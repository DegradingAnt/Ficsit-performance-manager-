// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMDistanceFieldAudit.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

// Owns AAbstractInstanceManager::InstanceMap and FInstanceComponentData. This is the only source of the
// AUTHORED distance-field intent — the component itself keeps no record of it. AbstractInstance is
// already a public dependency of this module (FicsitsPerformanceManager.Build.cs:62).
#include "AbstractInstanceManager.h"
#include "InstanceData.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Async/ParallelFor.h"

/*
 * ★ THE REPAIR IS OFF BY DEFAULT, AND THAT IS A PERFORMANCE DECISION RATHER THAN CAUTION.
 *
 * Flipping bAffectDistanceFieldLighting back on is one line per component. Doing it to every instanced
 * mesh in a factory the size of Ant's makes each one start contributing to distance-field lighting and
 * shadow work it currently skips. Ant: "Keep the performance good." Turning a visual bug into a frame
 * cost silently would be this mod failing at its own job.
 *
 * So: audit first, get a number, then decide. When the repair does run it says how many it touched.
 */
static TAutoConsoleVariable<int32> CVarDistanceFieldRepair(
	TEXT("FPM.DistanceField.Repair"), 0,
	TEXT("Re-enable distance-field contribution on instanced meshes that are AUTHORED to have it and do "
	     "not. 0 = audit only (default), 1 = repair and report the count. It never touches a mesh CSS "
	     "authored with distance fields off, and refuses entirely if no AAbstractInstanceManager was "
	     "found to read intent from. This ADDS renderer work - read the audit line first."),
	ECVF_Default);

namespace
{
	/*
	 * When to look. The world streams in over time, so a single sample cannot tell a settled state from
	 * a transient one. The marks spread out to 15 minutes.
	 *
	 * ⚠ EXTENDED FROM THREE TO FIVE ON 2026-08-10 TO SEPARATE A LAG FROM A LEAK, AND THAT WAS THE WRONG
	 * QUESTION. It was neither. The raw count rises and falls with the number of components loaded, and
	 * the fraction stays put — measured 31.97%->32.00% in one session and 37.10%->37.34% in another. So
	 * the marks are kept, but for a different reason: what they now watch is the DEFECT count, which has
	 * no streaming component in it and should simply never move.
	 *
	 * The five samples still earn their place. A defect count that is stable at 10 s and stable at 15
	 * minutes is a much stronger statement than one reading taken during the load.
	 */
	constexpr float GFPMDfSamplesSec[] = { 10.f, 30.f, 90.f, 300.f, 900.f };
	int32 GFPMDfSampleIndex = 0;
	float GFPMDfElapsed = 0.f;
	FTSTicker::FDelegateHandle GFPMDfTicker;
	TWeakObjectPtr<UWorld> GFPMDfWorld;

	int32 GFPMDfLastMissing = -1;

	struct FFPMDfCount
	{
		int32 Components = 0;
		int32 Missing = 0;          // bAffectDistanceFieldLighting == false
		int64 MissingInstances = 0; // instance count carried by those components — the visible weight
		int32 Repaired = 0;

		/*
		 * ★ THE MISSING COUNT SPLIT BY PROVENANCE, because until 2026-08-10 it was one number that
		 * conflated a content decision with a defect and could not be acted on either way.
		 *
		 * These four sum to `Missing`. Only `ShouldContribute` is a finding; only it is ever repaired.
		 */
		int32 MissingAuthoredOff = 0;
		int32 MissingLazyPoisoned = 0;
		int32 MissingShouldContribute = 0;
		int32 MissingForeign = 0;

		int64 ShouldContributeInstances = 0;

		/**
		 * How many `AAbstractInstanceManager` actors the provenance pass found.
		 *
		 * ⚠ ZERO MEANS NOT MEASURED, NOT "EVERYTHING IS FOREIGN". Without a manager every component
		 * falls into the Foreign bucket by default, which would read as "nothing to see" — the exact
		 * absence-as-clean-bill-of-health shape this project keeps paying for. The report says so out
		 * loud and the repair refuses to run.
		 */
		int32 Managers = 0;

		/** Entries in the provenance map. Non-zero is the LIVENESS PROOF that the InstanceMap read worked. */
		int32 ProvenanceEntries = 0;

		/*
		 * ★ THE COST OF EACH PHASE, BECAUSE "WE PARALLELISED IT" IS A CLAIM.
		 *
		 * Only ANALYSE runs on the other cores. If GATHER dominates — and it may, since it walks every
		 * actor in the level — then the parallel phase saved a slice of a small number, and the honest
		 * report says so. A single total would let a reader assume a win the numbers do not support.
		 */
		double GatherMs = 0.0;
		double AnalyseMs = 0.0;
		double RepairMs = 0.0;

		/**
		 * How many worker contexts `ParallelForWithTaskContext` actually created.
		 *
		 * ⚠ THIS IS THE LIVENESS PROOF FOR THE PARALLELISM, and it is the only thing in the report that
		 * can falsify it. One context means the loop ran entirely on the calling thread — which the
		 * engine is free to do, and which would make every "parallel" claim here false for that run. A
		 * report that cannot tell "16 workers" from "1" cannot tell you whether the rewrite did anything.
		 */
		int32 WorkerContexts = 0;
	};

	FFPMDfCount CountAndMaybeRepair(UWorld* World, bool bRepair, TArray<FString>& OutWorstNames)
	{
		FFPMDfCount C;
		if (World == nullptr) { return C; }

		/*
		 * Every ISM component in the world, not only AbstractInstance's. The manager's own map is
		 * private, and going through the actor iterator also catches instanced meshes owned by other
		 * mods — which is the honest scope for a question phrased as "does anything the player built
		 * fail to contribute".
		 */
		/*
		 * ★ THREE PHASES, AND ONLY THE MIDDLE ONE CAN USE THE OTHER CORES. Ant, 2026-08-10: *"do the
		 * parallise stuff for everything that CAN be done like that"* — the honest reading of CAN is what
		 * this split encodes, because two thirds of this function must stay serial and saying so is more
		 * useful than a blanket claim that the audit is now parallel.
		 *
		 *  1. GATHER — serial, game thread. `TActorIterator` is a stateful cursor over the level's actor
		 *     arrays and there is no parallel form of it. This phase cannot move, at all.
		 *  2. ANALYSE — `ParallelForWithTaskContext`, across every core. READS ONLY. Safe because the game
		 *     thread blocks inside the loop, so GC cannot run and free a component underneath a worker —
		 *     that is the whole reason UObject reads are legal here and would NOT be on a background
		 *     thread (`ue-async-threading`).
		 *  3. REPAIR — serial, game thread, and only over the offenders phase 2 found.
		 *     ⚠ `MarkRenderStateDirty()` touches render state and is NOT thread-safe. Repairing inside the
		 *     parallel loop would be a data race on the renderer, which is a far worse bug than the one
		 *     this audit exists to find.
		 *
		 * ★ AND IT TIMES ALL THREE, because "we parallelised it" is a claim and this project does not ship
		 * those unmeasured. If GATHER dominates, the parallelism bought nothing and the log will say so
		 * plainly rather than letting the next reader assume the win.
		 */
		const double TGatherStart = FPlatformTime::Seconds();

		TArray<UInstancedStaticMeshComponent*> AllComps;
		AllComps.Reserve(4096);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UInstancedStaticMeshComponent*> Comps;
			It->GetComponents<UInstancedStaticMeshComponent>(Comps);
			AllComps.Append(Comps);
		}

		/*
		 * Provenance, built here in GATHER because it walks actors and must be on the game thread. From
		 * this point it is READ-ONLY, which is what makes it safe to look up from the parallel phase —
		 * concurrent TMap::Find with no writer is fine, and there is no writer after this line.
		 */
		TMap<const UInstancedStaticMeshComponent*, EFPMDfProvenance> Provenance;
		C.Managers = FFPMDistanceFieldAudit::BuildProvenance(World, Provenance);
		C.ProvenanceEntries = Provenance.Num();

		const double TAnalyseStart = FPlatformTime::Seconds();
		C.GatherMs = (TAnalyseStart - TGatherStart) * 1000.0;

		/** Per-worker workspace. Mutated without synchronisation because each task owns one. */
		struct FDfCtx
		{
			int32 Components = 0;
			int32 Missing = 0;
			int64 MissingInstances = 0;
			int32 AuthoredOff = 0;
			int32 LazyPoisoned = 0;
			int32 ShouldContribute = 0;
			int32 Foreign = 0;
			int64 ShouldContributeInstances = 0;
			TArray<TPair<int32, UInstancedStaticMeshComponent*>> Offenders;
		};

		TArray<FDfCtx> Contexts;
		ParallelForWithTaskContext(Contexts, AllComps.Num(),
			[&AllComps, &Provenance](FDfCtx& Ctx, int32 Index)
			{
				UInstancedStaticMeshComponent* Comp = AllComps[Index];
				if (Comp == nullptr) { return; }
				++Ctx.Components;

				if (Comp->bAffectDistanceFieldLighting) { return; }

				// GetInstanceCount() reads a count off the component. No allocation, no render command,
				// no UObject creation — the three things that would make this unsafe here.
				const int32 Instances = Comp->GetInstanceCount();
				++Ctx.Missing;
				Ctx.MissingInstances += Instances;

				/*
				 * ★ CLASSIFY, DO NOT JUST COUNT. A component absent from the map is Foreign by
				 * definition — AbstractInstance does not own it, so its authored intent is unknown and
				 * nothing here is entitled to an opinion about it.
				 */
				const EFPMDfProvenance* Found = Provenance.Find(Comp);
				const EFPMDfProvenance P = Found != nullptr ? *Found : EFPMDfProvenance::Foreign;

				switch (P)
				{
				case EFPMDfProvenance::AuthoredOff:      ++Ctx.AuthoredOff; break;
				case EFPMDfProvenance::LazyPoisoned:     ++Ctx.LazyPoisoned; break;
				case EFPMDfProvenance::ShouldContribute: ++Ctx.ShouldContribute;
				                                         Ctx.ShouldContributeInstances += Instances; break;
				default:                                 ++Ctx.Foreign; break;
				}

				/*
				 * ⚠ ONLY REAL DEFECTS GET NAMED IN THE "worst" LINES. Listing the biggest authored-off
				 * meshes would put a foliage mesh at the top of a list headed "worst offenders" and send
				 * the next reader chasing content that is behaving exactly as designed.
				 *
				 * ⚠ POINTERS ONLY. Resolving GetStaticMesh()->GetName() would build an FString per
				 * offender on a worker; the names are wanted for at most five log lines, so they are
				 * resolved on the game thread below where that is unambiguously safe.
				 */
				if (P == EFPMDfProvenance::ShouldContribute && Ctx.Offenders.Num() < 64)
				{
					Ctx.Offenders.Emplace(Instances, Comp);
				}
			});

		TArray<TPair<int32, UInstancedStaticMeshComponent*>> Offenders;
		for (const FDfCtx& Ctx : Contexts)
		{
			C.Components += Ctx.Components;
			C.Missing += Ctx.Missing;
			C.MissingInstances += Ctx.MissingInstances;
			C.MissingAuthoredOff += Ctx.AuthoredOff;
			C.MissingLazyPoisoned += Ctx.LazyPoisoned;
			C.MissingShouldContribute += Ctx.ShouldContribute;
			C.MissingForeign += Ctx.Foreign;
			C.ShouldContributeInstances += Ctx.ShouldContributeInstances;
			Offenders.Append(Ctx.Offenders);
		}

		const double TRepairStart = FPlatformTime::Seconds();
		C.AnalyseMs = (TRepairStart - TAnalyseStart) * 1000.0;
		C.WorkerContexts = Contexts.Num();

		/*
		 * Phase 3. Serial by necessity — `MarkRenderStateDirty()` is not thread-safe.
		 *
		 * ⚠ AND IT WALKS EVERY COMPONENT, NOT THE OFFENDER LIST, WHICH LOOKS WRONG AND IS NOT. Each
		 * worker context caps its `Offenders` at 64, so that list is a SAMPLE for the "worst" log lines
		 * and never the complete set. Repairing from it would silently fix the first 64-per-worker and
		 * leave the rest broken, while the report claimed the world was repaired. Re-testing the flag
		 * here is one bool compare per component and it is the only way to be complete.
		 */
		/*
		 * ⚠⚠ AND IT REPAIRS ONLY `ShouldContribute`. THE OLD VERSION REPAIRED EVERY CLEAR FLAG IT FOUND,
		 * WHICH WAS NOT AGGRESSIVE — IT WAS WRONG.
		 *
		 * On Ant's save that is ~20,000 components carrying ~1,000,000 instances, and 32-37% of them are
		 * `bCastDistanceFieldShadows = false` in the CONTENT (`InstanceData.h:67`, EditDefaultsOnly).
		 * Setting those would override CSS's own per-mesh cost decisions wholesale, on a GPU already
		 * measured at 98%. Nobody would have seen it happen: the report said "REPAIRED n" either way.
		 *
		 * A Foreign component is not repaired either. Its authored intent is unknown, and writing render
		 * state on another mod's component off a guess is exactly the class of change this mod exists to
		 * argue against.
		 */
		if (bRepair && C.Managers > 0)
		{
			for (UInstancedStaticMeshComponent* Comp : AllComps)
			{
				if (Comp == nullptr || Comp->bAffectDistanceFieldLighting) { continue; }

				const EFPMDfProvenance* Found = Provenance.Find(Comp);
				if (Found == nullptr || *Found != EFPMDfProvenance::ShouldContribute) { continue; }

				Comp->bAffectDistanceFieldLighting = true;
				Comp->MarkRenderStateDirty();
				++C.Repaired;
			}
		}
		else if (bRepair)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] distance-field: repair REFUSED - no AAbstractInstanceManager was found, so no "
				     "component's authored intent could be read. Repairing on that would be guessing, and "
				     "the thing it would guess at is a million instances of someone else's content "
				     "decision. This is a statement about the instrument, not about the world."));
		}
		C.RepairMs = (FPlatformTime::Seconds() - TRepairStart) * 1000.0;

		/*
		 * Name the biggest offenders by INSTANCE count, not by component count — one component holding
		 * 4,000 foundations matters more than forty holding one pipe each.
		 *
		 * ⚠ THE NAMES ARE RESOLVED HERE AND NOT IN THE WORKER, on purpose. `GetStaticMesh()->GetName()`
		 * builds an FString and follows a UObject pointer; doing that per offender inside the parallel
		 * loop would allocate on every worker to produce strings that at most five of are ever printed.
		 * Phase 2 carries pointers, this carries the cost, and only for the five that survive the sort.
		 */
		Offenders.Sort([](const TPair<int32, UInstancedStaticMeshComponent*>& A,
		                  const TPair<int32, UInstancedStaticMeshComponent*>& B)
			{ return A.Key > B.Key; });

		for (int32 i = 0; i < FMath::Min(5, Offenders.Num()); ++i)
		{
			UInstancedStaticMeshComponent* Comp = Offenders[i].Value;
			const UStaticMesh* Mesh = Comp != nullptr ? Comp->GetStaticMesh() : nullptr;
			OutWorstNames.Add(FString::Printf(TEXT("%s x%d"),
				Mesh != nullptr ? *Mesh->GetName() : TEXT("<no mesh>"), Offenders[i].Key));
		}

		return C;
	}

	/**
	 * ★ WHAT THE PARALLEL PASS ACTUALLY BOUGHT, printed beside every result.
	 *
	 * Ant asked for *"the parallise stuff for everything that CAN be done like that"*, and this is the
	 * half of that request that is easy to skip: proving it. One worker context means the engine ran the
	 * loop inline on the calling thread and there was no parallelism at all this run.
	 */
	void ReportCost(const FFPMDfCount& C)
	{
		const double Total = C.GatherMs + C.AnalyseMs + C.RepairMs;

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   cost %.1f ms total - gather %.1f (serial, TActorIterator has no parallel form), "
			     "analyse %.1f across %d worker context(s), repair %.1f (serial, MarkRenderStateDirty is "
			     "not thread-safe)."),
			Total, C.GatherMs, C.AnalyseMs, C.WorkerContexts, C.RepairMs);

		UE_CLOG(C.WorkerContexts <= 1, LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   ⚠ %d worker context(s) - the analyse phase ran INLINE on the calling thread, so "
			     "nothing was parallel this run. Expected when the component count is small."),
			C.WorkerContexts);

		UE_CLOG(C.WorkerContexts > 1 && C.GatherMs > C.AnalyseMs, LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   ⚠ gather (%.1f ms) cost more than analyse (%.1f ms), so the parallel phase is "
			     "NOT where this audit spends its time. Parallelising further here would buy little."),
			C.GatherMs, C.AnalyseMs);
	}

	void Report(const FFPMDfCount& C, const TArray<FString>& Worst, const TCHAR* When)
	{
		if (C.Components == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] distance-field audit (%s): ZERO instanced mesh components found. That is a "
				     "statement about the instrument, not about the world - the audit ran too early, or "
				     "there is no game world."), When);
			return;
		}

		if (C.Missing == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] distance-field audit (%s): all %d instanced mesh component(s) contribute to "
				     "distance fields. THE THEORY IS DEAD for this world - rain passing through walls and "
				     "light leaking on low settings are NOT missing distance fields, and both need a "
				     "different explanation."), When, C.Components);
			ReportCost(C);
			return;
		}

		/*
		 * ⚠ THE HEADLINE IS NOW Display, NOT Warning, AND THAT IS THE POINT OF THE WHOLE REWRITE.
		 * "32% of components have no distance field" is a fact about CSS's content, not a fault. The
		 * Warning below is reserved for the bucket that is actually a defect.
		 */
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] distance-field audit (%s): %d of %d instanced mesh component(s) do not contribute "
			     "to distance fields, carrying %lld instance(s). Split by PROVENANCE, because that number "
			     "on its own says nothing: %d authored off (correct, content decision), %d lazy-poisoned, "
			     "%d SHOULD contribute (%lld instance(s)), %d foreign/unjudged."),
			When, C.Missing, C.Components, C.MissingInstances,
			C.MissingAuthoredOff, C.MissingLazyPoisoned, C.MissingShouldContribute,
			C.ShouldContributeInstances, C.MissingForeign);

		/*
		 * ★ THE INSTRUMENT HAS TO SAY WHEN IT COULD NOT MEASURE. Without a manager every component falls
		 * to Foreign, and a report full of "unjudged" would otherwise read like a clean world.
		 */
		if (C.Managers == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ NO AAbstractInstanceManager FOUND, so every count above is UNJUDGED, not "
				     "clean. Provenance entries: %d. Nothing was classified and nothing can be repaired."),
				C.ProvenanceEntries);
		}
		else if (C.MissingAuthoredOff == 0 && C.Missing > 0)
		{
			/*
			 * The liveness proof, inverted. On this game the authored-off bucket is by far the largest,
			 * so a zero there while components are missing means the map read produced nothing useful —
			 * a renamed field, a changed ownership model, or an access transformer that stopped applying.
			 */
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ %d manager(s) and %d provenance entr(ies), but ZERO authored-off "
				     "components among %d missing. On this game that bucket should dominate, so treat "
				     "this classification as SUSPECT rather than as a finding."),
				C.Managers, C.ProvenanceEntries, C.Missing);
		}

		if (C.MissingLazyPoisoned > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ %d component(s) are LAZY-POISONED: authored to cast distance-field "
				     "shadows, but AbstractInstance stored false. That means "
				     "lightweightinstances.AllowLazySpawn got turned ON - it ships 0 (\"Temp disabled\"). "
				     "Vanilla's own re-enable pass CANNOT fix these: AbstractInstanceManager.cpp:489 "
				     "gates on the very field lazy loading cleared."),
				C.MissingLazyPoisoned);
		}

		if (C.MissingShouldContribute > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ %d component(s) carrying %lld instance(s) are authored to contribute, "
				     "stored to contribute, and still are NOT. THAT is the defect this audit exists to "
				     "find. FPM.DistanceField.Repair 1 fixes exactly these and nothing else."),
				C.MissingShouldContribute, C.ShouldContributeInstances);
		}
		else if (C.Managers > 0)
		{
			/*
			 * ⚠⚠ THIS ZERO IS STRUCTURAL, NOT MEASURED, AND SAYING SO IS THE DIFFERENCE BETWEEN A PROOF
			 * AND A DEAD INSTRUMENT.
			 *
			 * Every writer of bAffectDistanceFieldLighting reachable on this build was enumerated
			 * 2026-08-10. On a component AbstractInstance owns there are exactly two:
			 *
			 *   AbstractInstanceManager.cpp:305   = bCastDistanceFieldShadows   (at creation)
			 *   AbstractInstanceManager.cpp:493   = true                        (the dead lazy pass)
			 *
			 * So authored-true + stored-true means :305 set it TRUE and nothing can clear it. The bucket
			 * CANNOT be non-zero as the code stands. The five vanilla writers that set it false
			 * (FGBuildable.cpp:2314/:2350 USplineMeshComponent, :2372/:2391/:2410 UStaticMeshComponent,
			 * FGProductionIndicatorInstanceComponent.cpp:14 UFGColoredInstanceMeshProxy) are all on
			 * types that are NOT UInstancedStaticMeshComponent, so this audit never sees them at all.
			 *
			 * The bucket is kept anyway: it is what would NOTICE a third writer arriving in a future
			 * game patch. But a reader must never take this zero as evidence gathered in their world.
			 */
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   defect bucket 0 - and that is STRUCTURAL, not a measurement. On this build "
				     "only AbstractInstanceManager.cpp:305 and :493 can write the flag on a component "
				     "this audit can see, and both set it TRUE for an authored-true mesh. So the "
				     "distance-field explanation is dead BY CONSTRUCTION for rain through walls, light "
				     "through terrain and DF shadow pop-in - all three need a different cause, and no "
				     "boot was going to change that. A non-zero here would mean the game gained a new "
				     "writer, which is worth knowing."));
		}

		for (const FString& S : Worst)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM]   worst SHOULD-CONTRIBUTE: %s"), *S);
		}

		ReportCost(C);

		if (C.Repaired > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   REPAIRED %d component(s) - the SHOULD-CONTRIBUTE bucket only. ⚠ That ADDS "
				     "renderer work. Measure the frame cost before leaving FPM.DistanceField.Repair on."),
				C.Repaired);
		}
		else if (C.MissingShouldContribute > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   audit only. FPM.DistanceField.Repair 1 would fix the %d SHOULD-CONTRIBUTE "
				     "component(s) above - and ONLY those. It will not touch the %d authored off."),
				C.MissingShouldContribute, C.MissingAuthoredOff);
		}

		/*
		 * ⚠ THE OVERLAY SHOWS THE DEFECT COUNT, NOT THE RAW MISSING COUNT. It used to read
		 * "19996/53557 components missing DF" on a healthy world, which is a scary number for a
		 * non-finding, sitting permanently on screen.
		 */
		FPMOverlay::Post(TEXT("distance-field"),
			FString::Printf(TEXT("%d defect / %d authored-off / %d total comps"),
				C.MissingShouldContribute, C.MissingAuthoredOff, C.Components));
	}
}

FFPMDistanceFieldAudit& FFPMDistanceFieldAudit::Get()
{
	static FFPMDistanceFieldAudit Instance;
	return Instance;
}

int32 FFPMDistanceFieldAudit::BuildProvenance(
	UWorld* World, TMap<const UInstancedStaticMeshComponent*, EFPMDfProvenance>& OutProvenance)
{
	OutProvenance.Reset();
	if (World == nullptr) { return 0; }

	int32 Managers = 0;

	/*
	 * Iterate rather than ask for THE manager. There is normally one, but a mod is free to spawn its
	 * own, and a component owned by a second manager would otherwise land in Foreign and be reported as
	 * unjudgeable when its intent was readable all along.
	 */
	for (TActorIterator<AAbstractInstanceManager> It(World); It; ++It)
	{
		AAbstractInstanceManager* Mgr = *It;
		if (Mgr == nullptr) { continue; }
		++Managers;

		// InstanceMap is protected; reachable here through the AccessTransformers friend on this class.
		for (const TPair<FName, FInstanceComponentData>& Pair : Mgr->InstanceMap)
		{
			const FInstanceComponentData& Entry = Pair.Value;

			/*
			 * ★ TWO FLAGS, AND THE GAP BETWEEN THEM IS THE WHOLE DIAGNOSIS.
			 *
			 *   InstanceData.bCastDistanceFieldShadows  the AUTHORED intent (InstanceData.h:67,
			 *                                           UPROPERTY EditDefaultsOnly, default true)
			 *   Entry.bCastDistanceFieldShadows         what AbstractInstance DECIDED, which is
			 *                                           `!bIsLazyLoading && authored`
			 *                                           (AbstractInstanceManager.cpp:827)
			 *
			 * With lightweightinstances.AllowLazySpawn at its shipped 0 these are always equal, so
			 * LazyPoisoned should be empty. Computing it anyway costs one bool compare and is what makes
			 * the audit able to NOTICE if that cvar is ever turned on, rather than silently changing
			 * meaning.
			 */
			const bool bAuthored = Entry.InstanceData.bCastDistanceFieldShadows;
			const bool bStored = Entry.bCastDistanceFieldShadows;

			const EFPMDfProvenance P =
				!bAuthored ? EFPMDfProvenance::AuthoredOff :
				!bStored   ? EFPMDfProvenance::LazyPoisoned :
				             EFPMDfProvenance::ShouldContribute;

			for (const TObjectPtr<ULightweightHierarchicalInstancedStaticMeshComponent>& Comp
			     : Entry.InstancedStaticMeshComponents)
			{
				/*
				 * The pointer is a KEY and is never dereferenced, here or by any caller. That is what
				 * makes this safe without a lifetime guarantee: a component destroyed between this pass
				 * and the lookup can only fail to match, and a failed match reads as Foreign — which is
				 * "not judged", the conservative answer.
				 */
				if (Comp != nullptr) { OutProvenance.Add(Comp.Get(), P); }
			}
		}
	}

	return Managers;
}

void FFPMDistanceFieldAudit::Arm()
{
	/*
	 * ⚠ THE SCHEDULE IS BUILT FROM THE ARRAY, NOT TYPED OUT. The old line hardcoded elements 0..2 and
	 * kept printing "10/30/90 s" after the array grew to five marks — a log line quietly disagreeing
	 * with the code it describes, in the file whose whole subject is instruments that lie.
	 */
	FString Schedule;
	for (int32 i = 0; i < UE_ARRAY_COUNT(GFPMDfSamplesSec); ++i)
	{
		Schedule += FString::Printf(TEXT("%s%.0f"), i == 0 ? TEXT("") : TEXT("/"), GFPMDfSamplesSec[i]);
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] distance-field audit ARMED - READ ONLY by default, no hook. It samples at %s s after "
		     "each world load. It counts instanced mesh components not contributing to distance fields "
		     "and SPLITS them by provenance: authored-off is CSS's own content decision and not a fault, "
		     "and only the should-contribute bucket is a defect. Three symptoms hang on that bucket: rain "
		     "through walls (NS_Rain owns a QueryMeshDistanceFieldGPU, so if the wall is not IN the field "
		     "the query is innocent), light through terrain on low settings, and the parked DF shadow "
		     "pop-in."),
		*Schedule);
}

void FFPMDistanceFieldAudit::OnWorldLoad(UWorld* World)
{
	if (GFPMDfTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMDfTicker);
		GFPMDfTicker.Reset();
	}

	GFPMDfWorld = World;
	GFPMDfSampleIndex = 0;
	GFPMDfElapsed = 0.f;
	GFPMDfLastMissing = -1;
	if (World == nullptr) { return; }

	GFPMDfTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float Delta) -> bool
		{
			/*
			 * ⚠ A NULL WORLD MUST NOT END THE SAMPLER, AND THE FIRST VERSION LET IT.
			 *
			 * Measured on the 2026-08-10 boot: only the "early" sample ever printed. The 30 s and 90 s
			 * marks never fired, three minutes later — because this returned false the first tick the
			 * weak pointer came back null, and a ticker that returns false is gone for good. Losing the
			 * later samples costs the whole TREND, which is the only thing that separates "vanilla's
			 * re-enable pass is still working" from "these were missed" — i.e. the actual question.
			 *
			 * This is the same shape as the FPMEnclosure blocker the review caught an hour earlier, in
			 * the file written immediately after fixing it. So: re-resolve from the engine, and keep
			 * ticking. Only the sample marks end the run.
			 */
			UWorld* W = GFPMDfWorld.Get();
			if (W == nullptr && GEngine != nullptr)
			{
				W = GEngine->GetCurrentPlayWorld();
				if (W != nullptr) { GFPMDfWorld = W; }
			}
			if (W == nullptr) { return true; }

			/*
			 * The ticker runs on a short fixed interval and the SAMPLE MARKS are checked against elapsed
			 * time. FTSTicker repeats at whatever delay it was created with, so passing 10 s and
			 * returning true would sample at 10/20/30 -- not the 10/30/90 spread this needs. The spread
			 * is the point: it is what distinguishes "vanilla is still working through the world" from
			 * "these were missed".
			 */
			GFPMDfElapsed += Delta;
			if (GFPMDfSampleIndex >= UE_ARRAY_COUNT(GFPMDfSamplesSec)) { return false; }
			if (GFPMDfElapsed < GFPMDfSamplesSec[GFPMDfSampleIndex]) { return true; }

			TArray<FString> Worst;
			const FFPMDfCount C = CountAndMaybeRepair(W, CVarDistanceFieldRepair.GetValueOnGameThread() != 0, Worst);

			if (FPMDiag::IsOn(FPMDiag::EChannel::DistanceField))
			{
				/*
				 * The COMPARISON is the interesting part, not the number. A count that falls between
				 * samples means the vanilla re-enable pass is still working; a count that holds is the
				 * bug. Saying which of those happened is the whole reason for sampling three times.
				 */
				static const TCHAR* const SampleNames[] = {
					TEXT("early"), TEXT("mid"), TEXT("late"), TEXT("5-min"), TEXT("15-min") };
				const TCHAR* When = GFPMDfSampleIndex < UE_ARRAY_COUNT(SampleNames)
					? SampleNames[GFPMDfSampleIndex] : TEXT("later");

				Report(C, Worst, When);

				/*
				 * ⚠⚠ THIS TRENDED THE WRONG NUMBER, AND THE WRONG NUMBER MOVED FOR A REASON THAT WAS NOT
				 * A BUG. Corrected 2026-08-10, second correction in one day to the same eight lines.
				 *
				 * The first correction made it report direction instead of mere change, because the old
				 * text said "vanilla is fixing it" while the count rose. That was right as far as it
				 * went. What it still got wrong is that it trended `C.Missing` — the RAW count — and
				 * then attributed every movement to a repair pass that does not run on this build.
				 *
				 * Measured across two of Ant's sessions:
				 *     11:22 -> 11:41   3444/10771 -> 3273/10227     ratio 31.97% -> 32.00%
				 *     16:30 -> 16:35  19769/53287 -> 19996/53557    ratio 37.10% -> 37.34%
				 *
				 * One FELL and one ROSE, and both ratios sat still. The raw count was tracking the
				 * DENOMINATOR — components streaming in and out — and "ROSE" was reporting the player
				 * walking towards their factory. [[snapshot-is-not-a-measurement]] again, in the shape
				 * of a numerator without its denominator.
				 *
				 * So the trend now follows the DEFECT count, which is the only bucket that can move for
				 * a reason worth acting on, and the raw pair is printed beside it so the denominator is
				 * never invisible again.
				 */
				if (GFPMDfLastMissing >= 0 && C.MissingShouldContribute != GFPMDfLastMissing)
				{
					// Named DefectDelta, not Delta: the enclosing ticker lambda already takes a `float
					// Delta` and C4457 is warnings-as-errors territory in some targets.
					const int32 DefectDelta = C.MissingShouldContribute - GFPMDfLastMissing;
					UE_LOG(LogFicsitsPerformanceManager, Warning,
						TEXT("[FPM]   ⚠ DEFECT count moved %d -> %d (%+d) since the last sample, while "
						     "raw missing went %d/%d. A moving defect count is a real finding - it means "
						     "components authored to contribute are losing the flag as you play, which a "
						     "one-shot repair cannot win against."),
						GFPMDfLastMissing, C.MissingShouldContribute, DefectDelta, C.Missing, C.Components);
				}
				else if (GFPMDfLastMissing >= 0)
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM]   defect count UNCHANGED at %d since the last sample (raw missing "
						     "%d/%d, which moves with streaming and is not a fault)."),
						C.MissingShouldContribute, C.Missing, C.Components);
				}
			}

			GFPMDfLastMissing = C.MissingShouldContribute;
			++GFPMDfSampleIndex;

			/*
			 * One ticker, one second apart, firing only at the sample marks. The first draft re-armed by
			 * scheduling a second ticker from inside the first, which left a no-op delegate ticking for
			 * the rest of the session — a leak in the one file whose subject is not spending frames.
			 */
			return GFPMDfSampleIndex < UE_ARRAY_COUNT(GFPMDfSamplesSec);
		}),
		/*Interval*/ 1.0f);
}

void FFPMDistanceFieldAudit::Disarm()
{
	/*
	 * The sampler is a repeating ticker with world state captured in file-scope globals. Left running
	 * past Disarm it keeps calling CountAndMaybeRepair against a module that is shutting down, and
	 * FPMFixes::DisarmAll() would have reported this fix disarmed while it did so.
	 */
	if (GFPMDfTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMDfTicker);
		GFPMDfTicker.Reset();
	}
	GFPMDfWorld.Reset();
}

void FFPMDistanceFieldAudit::AuditNow()
{
	UWorld* World = GFPMDfWorld.Get();
	if (World == nullptr && GEngine != nullptr) { World = GEngine->GetCurrentPlayWorld(); }

	TArray<FString> Worst;
	const FFPMDfCount C = CountAndMaybeRepair(World, CVarDistanceFieldRepair.GetValueOnGameThread() != 0, Worst);
	Report(C, Worst, TEXT("on demand"));
}

static FAutoConsoleCommandWithOutputDevice GFPMDfAuditCmd(
	TEXT("FPM.DistanceField.Audit"),
	TEXT("Count instanced mesh components that are not contributing to distance fields, and name the "
	     "worst by instance count."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMDistanceFieldAudit::AuditNow();
	}));
