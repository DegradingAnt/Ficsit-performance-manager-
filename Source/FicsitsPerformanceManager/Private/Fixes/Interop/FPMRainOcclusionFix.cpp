// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMRainOcclusionFix.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMBoxCache.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"

#include "Buildables/FGBuildable.h"
#include "FGRainOcclusionActor.h"
#include "InstanceData.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectIterator.h"

/*
 * ★ TWO KILL SWITCHES, ADDED 2026-08-08 TO ISOLATE A SUSPECTED REGRESSION.
 *
 * Ant found a bush rendering red near a main factory and asked whether the sweep broke it. Two separate
 * things this fix does could plausibly reach an asset, and they need testing apart:
 *
 *   FPM.Rain.Sweep  — the load-time pass over EVERY AFGBuildable subclass. It calls GetDefaultObject()
 *                     on ~3,828 classes during the world-load CONSTRUCTION phase, which FORCE-CONSTRUCTS
 *                     any CDO that does not exist yet. That is a far larger intervention than the two
 *                     fields this fix writes, and it is entirely new.
 *   FPM.Rain.Hooks  — the lazy per-class repair at BeginPlay / AddShapeFromClass / AddShapeFromBuildable.
 *
 * Set either to 0 in the console and reload the save. Both default ON, so shipping behaviour is
 * unchanged; these exist so ONE boot can answer which half is responsible instead of costing a rebuild
 * per hypothesis. Registering our own cvars is fine — the project law bans WRITING vanilla cvars with
 * ECVF_SetByConsole, not declaring our own.
 */
static TAutoConsoleVariable<int32> CVarRainSweep(
	TEXT("FPM.Rain.Sweep"), 1,
	TEXT("1 = run the load-time rain-occlusion sweep over all buildable classes. 0 = skip it entirely."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarRainHooks(
	TEXT("FPM.Rain.Hooks"), 1,
	TEXT("1 = repair rain-occlusion boxes lazily from the hooks. 0 = the hooks observe and do nothing."),
	ECVF_Default);

namespace
{
	/** Which source produced a box. Replaces a `Source[0] == 'i'` string sniff the review flagged. */
	enum class EBoxSource : uint8 { None, InstanceData, Components, Cache };

	/**
	 * A box below this on ANY axis is treated as no geometry.
	 *
	 * ★ REVIEW FINDING [HIGH]. BoxSphereBounds.h:226-229 GetBox() builds TBox(Origin-Extent,
	 * Origin+Extent), which sets IsValid=1 EVEN FOR ZERO EXTENT, and Box.h:867-909 TransformBy
	 * preserves that. So `Bounds.IsValid` alone accepts a zero-volume box, writes it to the CDO, and
	 * locks the class out of ever being retried. A degenerate occlusion volume occludes nothing, so the
	 * class would look repaired and rain would still fall through it — the exact failure this fix
	 * exists to end, wearing a success log line.
	 *
	 * 1 cm. Real buildables are metres across; nothing legitimate is thinner than this on all axes.
	 */
	constexpr double GMinUsefulExtent = 1.0;

	bool IsUsableBox(const FBox& Box)
	{
		if (!Box.IsValid) { return false; }
		const FVector Size = Box.GetSize();
		return Size.X >= GMinUsefulExtent || Size.Y >= GMinUsefulExtent || Size.Z >= GMinUsefulExtent;
	}

	TSet<FName> GHandledClasses;
	FPMBoxCache GCache;
	FString GEnvironmentKey;
	bool bGCacheDirty = false;

	int32 GAppliedFromCache = 0;
	int32 GDerivedInstanceData = 0;
	int32 GDerivedComponents = 0;
	int32 GGeometryless = 0;
	/**
	 * ⚠ ATOMIC, and the irony of the original is the reason. This counter exists to measure the OFF-GAME-
	 * THREAD branch, and it was a bare int32 incremented from exactly that branch — an unsynchronised
	 * read-modify-write on the one path guaranteed to be concurrent. Found by review 2026-08-09.
	 * The fix's own instrument was the racy part.
	 */
	std::atomic<int32> GOffThreadSkips{0};

	/*
	 * ★ THE TWO SILENT SKIPS, NOW COUNTED. Found 2026-08-09 from Ant's second world load, which printed
	 *     cache HIT | 3679 classes examined | 0 from cache, 0 instance-data, 0 components, 0 none
	 * Every bucket zero, against a denominator of 3679. Nothing was broken — CDOs live for the whole
	 * process, so a class settled by the FIRST sweep genuinely needs no work on the second, and
	 * GHandledClasses correctly short-circuits it. But the REPORT could not say so: both early returns
	 * incremented nothing, so "we already did this" and "we did nothing" printed identically.
	 *
	 * That is the dead-instrument shape again, in its subtler form. The line is not incapable of being
	 * non-zero in general — it was non-zero on the first sweep — it is incapable of being non-zero on
	 * EVERY SWEEP AFTER THE FIRST, which is every sweep Ant will ever look at in a long session.
	 *
	 * With these, the four outcome buckets plus these two sum to the examined count, so the denominator
	 * is accounted for rather than merely printed. A residual would now be visible as arithmetic.
	 */
	int32 GAlreadySettled = 0;     // handled by an earlier sweep in THIS process
	int32 GNoRepairNeeded = 0;     // vanilla already gave it a usable occlusion box

	/*
	 * ★ THE SEVENTH EXIT, FOUND BY THE UNACCOUNTED CHECK ON ITS FIRST RUN. Ant's 0.5.1 boot printed
	 *     3678 classes examined | 220 instance-data, 353 components, 27 none, 0 already settled,
	 *     3030 needed no repair | ⚠ 48 UNACCOUNTED
	 * 0+220+353+27+0+3030 = 3630 against 3678. Exactly the arithmetic the warning exists to surface,
	 * and it surfaced on the first sweep the check ever ran — which is the difference between this and
	 * the six dead instruments before it.
	 *
	 * The gap is the `if (!CDO)` guard: 48 classes come back from GetDerivedClasses(AFGBuildable) whose
	 * GetDefaultObject() does not Cast to AFGBuildable. They are NOT a defect on our side — a class can
	 * legitimately have no usable CDO — but "we could not look at it" and "we looked and it was fine"
	 * are different facts, and the sweep was reporting them as the same silence. Rain falling through a
	 * buildable whose class is in this bucket would otherwise be unexplainable from the log.
	 */
	int32 GNoUsableCDO = 0;

	/** Abstract / deprecated / superseded — skipped by the sweep loop before HandleClass ever sees them. */
	int32 GNotInstantiable = 0;

	TSet<FName> GReportedSkippedProfiles;

	/**
	 * ANT'S FILTER: "everything has collision. why not just make that do the rain interaction".
	 *
	 * DENY-LIST, NOT AN ALLOW-LIST. The full set of blocking profiles is project data we cannot
	 * enumerate, so an allow-list would silently exclude every profile we failed to think of — and
	 * excluding is the direction that REPRODUCES the bug. Failing open means an unknown profile
	 * occludes, which is the harmless error here.
	 */
	bool ProfileBlocksRain(const FName Profile)
	{
		if (Profile.IsNone()) { return true; }

		const FString Name = Profile.ToString();
		const bool bNonBlocking =
			   Name.Contains(TEXT("NoCollision"))
			|| Name.Contains(TEXT("Overlap"))
			|| Name.Contains(TEXT("Trigger"))
			|| Name.Contains(TEXT("Clearance"));

		if (bNonBlocking && !GReportedSkippedProfiles.Contains(Profile))
		{
			GReportedSkippedProfiles.Add(Profile);
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] rain: collision profile '%s' treated as non-blocking — its instances will not "
				     "occlude rain"), *Name);
		}
		return !bNonBlocking;
	}

	/**
	 * ★ ANT'S COLLISION FILTER, NOW ON THIS PATH TOO — IT WAS ONLY GUARDING THE INSTANCE-DATA SOURCE.
	 *
	 * Found 2026-08-08 when she asked why a bush had a rain box: ProfileBlocksRain was checked in
	 * CollectFromInstanceData and NOWHERE HERE, so the component path accepted any mesh that was not
	 * hidden. 429 of the 650 boxes written that session came through this function, i.e. the MAJORITY
	 * of the fix bypassed the filter whose entire job is "do not occlude for things that are not solid".
	 *
	 * Her framing is the correct test and it applies to both sources equally: rain should be stopped by
	 * what is SOLID. A decorative mesh with no collision is not solid, and giving it an occlusion volume
	 * makes rain stop at a bush — a new wrong behaviour, not a fix.
	 */
	void AccumulateMeshBounds(const UActorComponent* Comp, const FTransform& RelativeTo, FBox& OutBounds)
	{
		const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp);
		if (!SMC || !SMC->GetStaticMesh() || SMC->bHiddenInGame) { return; }

		if (SMC->GetCollisionEnabled() == ECollisionEnabled::NoCollision) { return; }
		if (!ProfileBlocksRain(SMC->GetCollisionProfileName())) { return; }

		OutBounds += SMC->GetStaticMesh()->GetBoundingBox().TransformBy(RelativeTo);
	}

	/** DEPTH-BOUNDED, and the increment IS the guard — the cap was once written and left inert because
	 *  the recursive call did not pass Depth + 1, so it READ like protection while doing nothing. */
	void WalkScsNode(const USCS_Node* Node, const FTransform& ParentToRoot, FBox& OutBounds, int32 Depth = 0)
	{
		if (!Node) { return; }

		static constexpr int32 MaxScsDepth = 64;
		if (Depth >= MaxScsDepth)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] rain: SCS depth cap (%d) hit — bounds may be incomplete for this class"), MaxScsDepth);
			return;
		}

		FTransform NodeToRoot = ParentToRoot;
		if (const USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate))
		{
			// Child-first: UE's `A * B` applies A then B, so this reads "my space -> root space".
			NodeToRoot = SceneTemplate->GetRelativeTransform() * ParentToRoot;
		}
		AccumulateMeshBounds(Node->ComponentTemplate, NodeToRoot, OutBounds);

		for (const USCS_Node* Child : Node->GetChildNodes())
		{
			WalkScsNode(Child, NodeToRoot, OutBounds, Depth + 1);
		}
	}

	/** Chain-to-root of a component, stopping AT the root (whose own transform is actor placement). */
	FTransform ComponentChainToRoot(const USceneComponent* Comp, const USceneComponent* RootComp)
	{
		FTransform ToRoot = FTransform::Identity;
		for (const USceneComponent* C = Comp; C && C != RootComp; C = C->GetAttachParent())
		{
			ToRoot = ToRoot * C->GetRelativeTransform();
		}
		return ToRoot;
	}

	/**
	 * The transform an SCS root node should START from.
	 *
	 * Identity when it hangs off the actor root, but a node with bIsParentComponentNative is attached to
	 * a NATIVE component named by ParentComponentOrVariableName — so its subtree must be seeded with
	 * that component's own offset or the whole subtree is misplaced.
	 */
	FTransform NativeParentToRoot(UClass* Class, const USCS_Node* Node)
	{
		if (!Node || !Node->bIsParentComponentNative || Node->ParentComponentOrVariableName.IsNone())
		{
			return FTransform::Identity;
		}

		const AActor* CDO = Cast<AActor>(Class->GetDefaultObject());
		if (!CDO) { return FTransform::Identity; }

		const USceneComponent* RootComp = CDO->GetRootComponent();
		for (const UActorComponent* Comp : CDO->GetComponents())
		{
			if (Comp && Comp->GetFName() == Node->ParentComponentOrVariableName)
			{
				if (const USceneComponent* Scene = Cast<USceneComponent>(Comp))
				{
					return ComponentChainToRoot(Scene, RootComp);
				}
			}
		}
		return FTransform::Identity;
	}

	/** SOURCE B (fallback): CDO components + SCS templates. Two traversals because a blueprint CDO does
	 *  not carry its blueprint-created components — those exist only as unattached SCS templates. */
	FBox CollectFromComponents(UClass* Class)
	{
		FBox Bounds(ForceInit);

		if (const AActor* CDO = Cast<AActor>(Class->GetDefaultObject()))
		{
			const USceneComponent* RootComp = CDO->GetRootComponent();
			for (const UActorComponent* Comp : CDO->GetComponents())
			{
				FTransform ToRoot = FTransform::Identity;
				if (const USceneComponent* Scene = Cast<USceneComponent>(Comp))
				{
					// Stop AT the root: its own relative transform is the ACTOR's placement.
					for (const USceneComponent* C = Scene; C && C != RootComp; C = C->GetAttachParent())
					{
						ToRoot = ToRoot * C->GetRelativeTransform();
					}
				}
				AccumulateMeshBounds(Comp, ToRoot, Bounds);
			}
		}

		for (UClass* C = Class; C; C = C->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(C))
			{
				if (BPGC->SimpleConstructionScript)
				{
					for (const USCS_Node* RootNode : BPGC->SimpleConstructionScript->GetRootNodes())
					{
						/*
						 * REVIEW FINDING [MED]: an SCS root node may be attached to a NATIVE parent
						 * component rather than to the actor root, and seeding at Identity silently
						 * discards that parent's offset. Failure scenario from the review: a native base
						 * declares MeshRoot at (0,0,400) and the blueprint hangs its meshes under it —
						 * the box comes out 400 units low, so rain occludes below the building and falls
						 * through the roof.
						 */
						WalkScsNode(RootNode, NativeParentToRoot(Class, RootNode), Bounds);
					}
				}
			}
		}
		return Bounds;
	}

	/** SOURCE A (primary): the AbstractInstance data object — where lightweight buildables keep their
	 *  real geometry, because FGColoredInstanceMeshProxy carries no StaticMesh of its own. */
	FBox CollectFromInstanceData(const AFGBuildable* CDO, int32& OutConsidered, int32& OutSkipped)
	{
		FBox Bounds(ForceInit);
		OutConsidered = 0;
		OutSkipped = 0;

		const UAbstractInstanceDataObject* Data = CDO->GetLightweightInstanceData();
		if (!Data) { return Bounds; }

		// BY VALUE (InstanceData.h:313) — bind once. The review flagged a deep copy per call.
		const TArray<FInstanceData> Instances = Data->GetInstanceData();
		for (const FInstanceData& Instance : Instances)
		{
			if (!Instance.StaticMesh) { continue; }
			++OutConsidered;

			if (!ProfileBlocksRain(Instance.CollisionProfileName)) { ++OutSkipped; continue; }

			Bounds += Instance.StaticMesh->GetBoundingBox().TransformBy(Instance.RelativeTransform);
		}
		return Bounds;
	}

	/**
	 * Derive a class's box from scratch. Returns EBoxSource::None when nothing usable was found.
	 *
	 * ★ REVIEW FINDING: the two sources are UNIONED, not exclusive. Running B only when A produced
	 * nothing means a class with both instance geometry AND a native mesh component on the CDO gets
	 * only A's contribution — and UNDERSIZING is the direction that reproduces the bug. Both are cheap
	 * and this runs once per class.
	 */
	EBoxSource DeriveBox(AFGBuildable* CDO, FBox& OutBox)
	{
		int32 Considered = 0, Skipped = 0;
		const FBox FromInstances = CollectFromInstanceData(CDO, Considered, Skipped);
		const FBox FromComponents = CollectFromComponents(CDO->GetClass());

		OutBox = FBox(ForceInit);
		if (FromInstances.IsValid)  { OutBox += FromInstances; }
		if (FromComponents.IsValid) { OutBox += FromComponents; }

		if (!IsUsableBox(OutBox)) { return EBoxSource::None; }

		// Label for the log only. Both may have contributed; report the one that carried the geometry.
		if (IsUsableBox(FromInstances)) { return EBoxSource::InstanceData; }
		return EBoxSource::Components;
	}

	/**
	 * ★ REVIEW FINDING [MED] — CDO INHERITANCE. A subclass CDO created after we repaired its parent
	 * INHERITS the parent's box, so the IsValid eligibility check sees a valid box and skips the class
	 * — leaving the subclass wearing its parent's geometry. Detect it: a box identical to a superclass
	 * box that WE wrote is inherited, not authored.
	 */
	bool BoxIsInheritedFromUs(const AFGBuildable* CDO)
	{
		for (UClass* Super = CDO->GetClass()->GetSuperClass(); Super; Super = Super->GetSuperClass())
		{
			const AFGBuildable* SuperCDO = Cast<AFGBuildable>(Super->GetDefaultObject());
			if (!SuperCDO) { break; }
			// Same path-name key as HandleClass, or this lookup silently never matches.
			if (!GHandledClasses.Contains(FName(*Super->GetPathName()))) { continue; }
			if (SuperCDO->mRainOcclusionBoundingBox == CDO->mRainOcclusionBoundingBox) { return true; }
		}
		return false;
	}

	/** True when this class still needs a box. */
	bool NeedsRepair(const AFGBuildable* CDO)
	{
		if (!CDO->DoesAffectOcclusionSystem()) { return false; }
		if (CDO->GetOcclusionShape() != EFGRainOcclusionShape::ROCS_Box) { return false; }
		if (CDO->mRainOcclusionBoundingBox.IsValid && !BoxIsInheritedFromUs(CDO)) { return false; }
		return true;
	}

	/**
	 * Handle one class: cache hit, else derive, else opt out. Writes the CDO.
	 *
	 * THREAD GUARD LIVES HERE, NOT AT THE CALL SITES (my own review finding). Both callers checked
	 * IsInGameThread(), which meant the next caller to forget inherited a silent TSet race. The state
	 * this touches is what needs protecting, so the check belongs with the state.
	 */
	EBoxSource HandleClass(AFGBuildable* CDO, bool bFromSweep)
	{
		// Counted, not silent. The sweep passes Cast<AFGBuildable>(Class->GetDefaultObject()), which can
		// be null for a class with no usable CDO -- 48 of them on Ant's 0.5.1 boot.
		if (!CDO)
		{
			if (bFromSweep) { ++GNoUsableCDO; }
			return EBoxSource::None;
		}

		if (!IsInGameThread())
		{
			++GOffThreadSkips; // counted, not silent — a skip nobody records is a coverage gap
			return EBoxSource::None;
		}

		// bFromSweep is false for the three hook call sites, so FPM.Rain.Hooks gates them independently
		// of the sweep. Both default on.
		if (!bFromSweep && CVarRainHooks.GetValueOnGameThread() == 0) { return EBoxSource::None; }

		// ★ REVIEW FINDING: key on the PATH name, not the short name. Two buildable classes in different
		// packages can share a short name — `Build_Wall_C` from two different mods is not hypothetical
		// in a 65-mod install — and a collision would skip the second one forever.
		const FName ClassName(*CDO->GetClass()->GetPathName());
		// Counted only for sweep calls: the hooks fire between sweeps and would otherwise inflate the
		// next sweep's tally with work that did not belong to it.
		if (GHandledClasses.Contains(ClassName))
		{
			if (bFromSweep) { ++GAlreadySettled; }
			return EBoxSource::None;
		}
		if (!NeedsRepair(CDO))
		{
			if (bFromSweep) { ++GNoRepairNeeded; }
			return EBoxSource::None;
		}

		FBox Box(ForceInit);
		EBoxSource Source = EBoxSource::None;

		if (const FBox* Cached = GCache.Find(ClassName))
		{
			Box = *Cached;
			Source = EBoxSource::Cache;
		}
		else if (GCache.IsKnownGeometryless(ClassName))
		{
			Source = EBoxSource::None;
		}
		else
		{
			Source = DeriveBox(CDO, Box);
			bGCacheDirty = true;
			if (Source == EBoxSource::None) { GCache.AddGeometryless(ClassName); }
			else { GCache.Add(ClassName, Box); }
		}

		// Only NOW is the class settled. Marking it before the attempt (my own review finding) made a
		// transient failure permanent.
		GHandledClasses.Add(ClassName);

		if (Source == EBoxSource::None)
		{
			CDO->mAffectsOcclusion = false;
			const int32 N = ++GGeometryless;
			if (N == 1 || (N % 25) == 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] rain: %d class(es) had NO usable geometry and were opted OUT of occlusion "
					     "— latest %s. If rain still falls through this class, this line is why."),
					N, *ClassName.ToString());
			}
			return EBoxSource::None;
		}

		CDO->mRainOcclusionBoundingBox = FBox3f(FVector3f(Box.Min), FVector3f(Box.Max));

		switch (Source)
		{
		case EBoxSource::Cache:        ++GAppliedFromCache;    break;
		case EBoxSource::InstanceData: ++GDerivedInstanceData; break;
		case EBoxSource::Components:   ++GDerivedComponents;   break;
		default: break;
		}
		return Source;
	}
}

FFPMRainOcclusionFix& FFPMRainOcclusionFix::Get()
{
	static FFPMRainOcclusionFix Instance;
	return Instance;
}

void FFPMRainOcclusionFix::OnWorldLoad(UWorld* World)
{
	/*
	 * THE SWEEP. Once per world load, in the CONSTRUCTION phase, with the loading screen up.
	 *
	 * Ant: "that way we can do the sweep as you wanted when needed only and keep whats keepable."
	 * A cache hit costs a file read and a map lookup per class. A miss costs one derivation per class,
	 * paid at the one moment a hitch is invisible, and is then kept.
	 */
	// Counters are per-sweep from here on. They were file-static and cumulative, so the second world
	// load in a session re-printed the FIRST sweep's totals and read as a fresh result. Found on the
	// 2026-08-08 reload: a "cache HIT" line showing the MISS run's numbers.
	GAppliedFromCache = GDerivedInstanceData = GDerivedComponents = GGeometryless = 0;
	GAlreadySettled = GNoRepairNeeded = GNoUsableCDO = 0;

	if (CVarRainSweep.GetValueOnGameThread() == 0)
	{
		FPMOverlay::Post(TEXT("rain sweep"), TEXT("SKIPPED — FPM.Rain.Sweep is 0"));
		return;
	}

	GEnvironmentKey = FPMBoxCache::ComputeEnvironmentKey();
	const bool bHit = GCache.Load(GEnvironmentKey);

	TArray<UClass*> BuildableClasses;
	GetDerivedClasses(AFGBuildable::StaticClass(), BuildableClasses, /*bRecursive*/ true);

	/*
	 * ★ THE 48. Found by the UNACCOUNTED check, then found AGAIN when my first fix for it was wrong.
	 *
	 * Ant's 0.5.1 boot printed `** 48 UNACCOUNTED **`. I attributed it to HandleClass's `!CDO` guard
	 * and added a counter there. Her 0.5.3 boot printed `0 no usable CDO | ** 48 UNACCOUNTED **` --
	 * the same 48, and my explanation disproved by the very counter I added to confirm it.
	 *
	 * The skip is HERE, one line above HandleClass, and it never reached the function I was auditing:
	 * abstract, deprecated and superseded classes are `continue`d out of the loop while still counting
	 * toward `BuildableClasses.Num()`. Correct behaviour -- an abstract class has no instances to
	 * occlude rain -- but it was silent, and silence is what made 48 classes unaccountable.
	 *
	 * Worth keeping as a note on method: the arithmetic said 48 were missing and was right BOTH times.
	 * My first guess at WHERE was wrong, and only a counter that could disprove it revealed that.
	 */
	int32 LocalNotInstantiable = 0;
	for (UClass* Class : BuildableClasses)
	{
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			++LocalNotInstantiable;
			continue;
		}
		HandleClass(Cast<AFGBuildable>(Class->GetDefaultObject()), /*bFromSweep*/ true);
	}
	GNotInstantiable = LocalNotInstantiable;

	// Posted to the overlay AND the log — this is the line Ant wants to see on the loading screen to
	// know the fix ran and what it actually did.
	/*
	 * EVERY EXAMINED CLASS LANDS IN A BUCKET, and the line says so by printing the residual. Six outcomes
	 * are exhaustive by construction -- HandleClass has exactly six exits for a sweep call -- so a
	 * non-zero "unaccounted" is a real defect surfacing as arithmetic rather than as silence. That is the
	 * property the previous line lacked: it printed a denominator of 3679 against four zeros and looked
	 * like a broken sweep when nothing was wrong.
	 */
	const int32 Examined = BuildableClasses.Num();
	const int32 Bucketed = GAppliedFromCache + GDerivedInstanceData + GDerivedComponents
	                     + GGeometryless + GAlreadySettled + GNoRepairNeeded + GNoUsableCDO
	                     + GNotInstantiable;
	FString Summary = FString::Printf(
		TEXT("cache %s | %d classes examined | %d from cache, %d instance-data, %d components, %d none, "
		     "%d already settled, %d needed no repair, %d no usable CDO, %d not instantiable"),
		bHit ? TEXT("HIT") : TEXT("MISS->rebuilt"),
		Examined, GAppliedFromCache, GDerivedInstanceData, GDerivedComponents, GGeometryless,
		GAlreadySettled, GNoRepairNeeded, GNoUsableCDO, GNotInstantiable);
	if (Bucketed != Examined)
	{
		// ASCII, not '⚠'. The overlay's Mono font rendered the warning sign as a tofu box on Ant's
		// 0.5.1 boot -- a warning nobody can read is not a warning, and this is the one line whose
		// whole job is to be noticed.
		Summary += FString::Printf(TEXT(" | ** %d UNACCOUNTED **"), Examined - Bucketed);
	}
	if (GOffThreadSkips.load() > 0)
	{
		// Stated, never silent -- an off-thread skip is a coverage gap, not a no-op.
		//
		// ⚠ AND LABELLED "this session", because unlike every other counter here it is NOT reset per
		// sweep. That is deliberate: off-thread skips come from the HOOK call sites, which fire between
		// sweeps, so a per-sweep reset would throw the interesting ones away. But printing a cumulative
		// figure unlabelled beside six per-sweep ones is exactly the defect the comment above the reset
		// line records -- a second sweep re-showing the first sweep's numbers as though they were fresh.
		// Caught on the review pass for this bump, before it shipped. Same bug, opposite direction.
		Summary += FString::Printf(TEXT(" | %d off-thread skip(s) this session"), GOffThreadSkips.load());
	}
	FPMOverlay::Post(TEXT("rain sweep"), Summary);

	if (bGCacheDirty)
	{
		GCache.Save(GEnvironmentKey);
		bGCacheDirty = false;
	}

	/*
	 * ⚠ THE SWEEP ONLY SEES LOADED CLASSES. Blueprint buildables load lazily, so a class whose assets
	 * arrive later this session is not in the list above. That is what the hooks are for now — they are
	 * the MISS-HANDLER, not the mechanism.
	 */
}

void FFPMRainOcclusionFix::Arm()
{
	AFGBuildable* Sample = GetMutableDefault<AFGBuildable>();

	/*
	 * HOOK 1 — a class the sweep never saw, caught as its first instance comes alive.
	 *
	 * ★ REVIEW FINDING [HIGH], AND IT WAS A REGRESSION I INTRODUCED. Repairing only the CDO does not
	 * reach THIS instance: UObject copies its archetype at construction, so the live actor already
	 * holds the pre-repair invalid box, and the actor path registers via AddShapeFromBuildable
	 * (FGRainOcclusionActor.h:116) which reads the ACTOR. The old mod wrote both; I dropped the
	 * instance write. Write both.
	 */
	auto OnBuildableBeginPlay = [](auto& Scope, AFGBuildable* Buildable)
	{
		if (!Buildable) { return; }

		AFGBuildable* CDO = Cast<AFGBuildable>(Buildable->GetClass()->GetDefaultObject());
		HandleClass(CDO, /*bFromSweep*/ false);

		if (CDO && CDO->mRainOcclusionBoundingBox.IsValid && !Buildable->mRainOcclusionBoundingBox.IsValid)
		{
			Buildable->mRainOcclusionBoundingBox = CDO->mRainOcclusionBoundingBox;
		}
		if (CDO && !CDO->mAffectsOcclusion)
		{
			Buildable->mAffectsOcclusion = false;
		}
	};

	/*
	 * HOOK 2 — lightweight instances, which never spawn as actors, so hook 1 never sees them. The rain
	 * system registers them from the CLASS instead.
	 *
	 * REVIEW FINDING [MED]: derive the CDO from the pointer's class rather than trusting that a
	 * parameter named BuildableCDO is one. If a non-CDO were ever passed, a const_cast write would land
	 * on an instance while the CLASS got marked handled.
	 */
	auto OnAddShapeFromClass = [](auto& Scope, const UObject* WorldContext, const AFGBuildable* BuildableCDO,
	                              const FSimpleOcclusionData& InstanceData, FRainHashKey Hash)
	{
		if (!BuildableCDO) { return; }
		HandleClass(Cast<AFGBuildable>(BuildableCDO->GetClass()->GetDefaultObject()), /*bFromSweep*/ false);
	};

	/*
	 * HOOK 3 — THE ACTOR REGISTRATION PATH, AND IT IS THE ONE THAT WAS MISSING.
	 *
	 * ★ REVIEW FINDING [HIGH]. AddShapeFromBuildable (FGRainOcclusionActor.h:116) takes the ACTOR, not
	 * the CDO — so the actor path reads the box off the live instance. UE copies archetype values at
	 * construction, strictly before BeginPlay, so every instance built during save load snapshots the
	 * INVALID box and hook 1's CDO repair arrives too late for all of them. That is why the old mod
	 * could log 1,300-2,700 "repairs" a session while rain kept falling through: it was repairing the
	 * class after the instances had already taken their copy.
	 *
	 * Repairing here, at the moment of registration, is the last point where it still counts.
	 */
	auto OnAddShapeFromBuildable = [](auto& Scope, const AFGBuildable* Buildable,
	                                  const FSimpleOcclusionData& InstanceData, FRainHashKey Hash)
	{
		if (!Buildable) { return; }

		AFGBuildable* Mutable = const_cast<AFGBuildable*>(Buildable);
		AFGBuildable* CDO = Cast<AFGBuildable>(Buildable->GetClass()->GetDefaultObject());
		HandleClass(CDO, /*bFromSweep*/ false);

		if (CDO && CDO->mRainOcclusionBoundingBox.IsValid && !Mutable->mRainOcclusionBoundingBox.IsValid)
		{
			Mutable->mRainOcclusionBoundingBox = CDO->mRainOcclusionBoundingBox;
		}
	};

	FPM_SUBSCRIBE_VIRTUAL("rain-occlusion", AFGBuildable::BeginPlay, Sample, OnBuildableBeginPlay);
	FPM_SUBSCRIBE("rain-occlusion", URainOcclusionWorldSubsystem::AddShapeFromClass, OnAddShapeFromClass);
	FPM_SUBSCRIBE("rain-occlusion", URainOcclusionWorldSubsystem::AddShapeFromBuildable, OnAddShapeFromBuildable);
}
