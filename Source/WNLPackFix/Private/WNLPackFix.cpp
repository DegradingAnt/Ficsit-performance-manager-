#include "WNLPackFix.h"
#include "WNLPerfGovernor.h"
#include "WNLFogController.h"

#include "Patching/NativeHookManager.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Buildables/FGBuildable.h"
#include "FGRainOcclusionActor.h"
#include "FGColoredInstanceMeshProxy.h"
#include "FGConveyorItemSubSystem.h"
#include "FGConveyorInstanceMeshBucket.h"
#include "AkGameplayStatics.h"
#include "Creature/FGNavMeshes.h"
#include "NavMesh/RecastNavMesh.h"   // direct TileNumberHardLimit access (AccessTransformers friend)
#include "FGFoliageInstancedSMC.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Misc/CoreDelegates.h"
#include "GameFramework/InputSettings.h"
#include "Containers/Ticker.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include <atomic>

DEFINE_LOG_CATEGORY(LogWNLPackFix);

FBox WNLPackFix_CollectLocalMeshBounds(UClass* Class);

void FWNLPackFixModule::StartupModule()
{
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] v0.9.9 loading"));

	// Native hooks must never be armed inside the Unreal Editor (crash risk per SML docs);
	// runtime `if` keeps the code compiled everywhere so errors surface early.
	if (!WITH_EDITOR)
	{
		RegisterStatsSignRpcGate();
		RegisterStaticBaseFix();
		RegisterRainOcclusionFix();
		RegisterContactShadowSuppressor(); // client-only; self-guards dedicated server
		RegisterWwiseServerAudioGate();    // server-only; self-guards clients
		RegisterNavMeshCoverageFix();      // raises tile ceiling so creatures path the whole map

		// RAW MOUSE (Ant): UE ships hidden mouse smoothing ON; an Engine.ini override gets wiped
		// whenever the game rewrites its Saved configs (observed 2026-07-17 — our earlier ini fix
		// vanished). Setting the InputSettings CDO here survives every config rewrite and ships
		// to everyone using the mod. UPlayerInput reads this each frame, so it applies live.
		if (!IsRunningDedicatedServer())
		{
			if (UInputSettings* Input = GetMutableDefault<UInputSettings>())
			{
				if (Input->bEnableMouseSmoothing)
				{
					Input->bEnableMouseSmoothing = false;
					UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] mouse smoothing disabled -> raw input"));
				}
				else
				{
					UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] mouse smoothing already off (raw input)"));
				}
			}
		}

		FWNLPerfGovernor::Get().Start();  // no-ops on dedicated servers
		FWNLFogController::Get().Start();  // no-ops on dedicated servers

		// In-game config MENU: registered declaratively by UWNLRootInstanceModule (the SML-documented
		// root-module pattern — see WNLRootInstanceModule.h). Nothing to do here.
	}
}

// Set once the suppression sweep below is armed. The governor refuses to force-enable contact
// shadows until this is true — the sweep is what makes them shimmer-safe (belt items + foliage).
bool GWNLContactShadowSweepArmed = false;

void FWNLPackFixModule::RegisterContactShadowSuppressor()
{
	if (IsRunningDedicatedServer())
	{
		return; // no render proxies on the server
	}
	// Contact shadows shimmer on two mover classes, and ONLY those get them suppressed —
	// everything static keeps its grounding contact shadows:
	//  1. Belt-item meshes (UFGConveyorInstanceMeshBucket): fast screen-space translation →
	//     temporal history rejected every frame.
	//  2. FOLIAGE (UFGFoliageInstancedSMC — v0.8.3 boot finding "trees looked super bad"):
	//     alpha-masked canopies turn screen-space contact shadows into crawling speckle.
	// bCastContactShadow defaults true, is per-primitive, and reaches the instanced draw
	// (PrimitiveSceneProxy copies it). Buckets spawn lazily and foliage streams with tiles, so a
	// low-frequency ticker re-applies; SetCastContactShadow early-returns when unchanged.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float /*Dt*/) -> bool
	{
		UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
		if (!World || !World->IsGameWorld())
		{
			return true;
		}
		int32 FlippedBelts = 0, FlippedFoliage = 0;
		// Belt items spawn lazily as new item TYPES first ride the belts → sweep the (cheap) subsystem
		// component list every pass.
		if (AFGConveyorItemSubsystem* Sub = AFGConveyorItemSubsystem::Get(World))
		{
			TArray<UFGConveyorInstanceMeshBucket*> Buckets;
			Sub->GetComponents(Buckets);
			for (UFGConveyorInstanceMeshBucket* B : Buckets)
			{
				if (B && B->bCastContactShadow)
				{
					B->SetCastContactShadow(false);
					++FlippedBelts;
				}
			}
		}
		// Foliage is a WHOLE-object-array scan (TObjectIterator), heavier than the belt list, and
		// foliage tiles stream in slowly and stay static once loaded — so only sweep it every 4th
		// pass (~20s) instead of every 5s. The first pass runs immediately (counter starts at 0).
		static int32 FoliageDivider = 0;
		if (FoliageDivider++ % 4 == 0)
		{
			// Iterate the BASE foliage class, not just UFGFoliageInstancedSMC — the first-boot ISM
			// census showed the world also has plain UFoliageInstancedStaticMeshComponent instances
			// that our FG-subclass-only sweep missed (they kept shimmering). The base catches both,
			// and only foliage extends it (buildables use UFGColoredInstanceMeshProxy).
			for (TObjectIterator<UFoliageInstancedStaticMeshComponent> It; It; ++It)
			{
				if (It->GetWorld() == World && It->bCastContactShadow)
				{
					It->SetCastContactShadow(false);
					++FlippedFoliage;
				}
			}
		}
		if (FlippedBelts > 0 || FlippedFoliage > 0)
		{
			UE_LOG(LogWNLPackFix, Display,
				TEXT("[WNLPackFix] contact shadows suppressed: %d belt bucket(s), %d foliage component(s)"),
				FlippedBelts, FlippedFoliage);
		}
		// One-shot census (boot-test instrumentation): what instanced-mesh classes actually render
		// this world, so foliage-like stragglers (landscape grass, BP-only foliage) can be added to
		// the suppression list from evidence. Fires on the first in-world pass REGARDLESS of whether
		// our UFGFoliageInstancedSMC guess matched — that mismatch is exactly what it must diagnose.
		static bool bCensusDone = false;
		if (!bCensusDone)
		{
			bCensusDone = true;
			TMap<FString, int32> ClassCounts;
			for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
			{
				if (It->GetWorld() == World)
				{
					ClassCounts.FindOrAdd(It->GetClass()->GetName())++;
				}
			}
			ClassCounts.ValueSort(TGreater<int32>());
			int32 Shown = 0;
			for (const auto& Pair : ClassCounts)
			{
				UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   ISM census: %5d x %s"), Pair.Value, *Pair.Key);
				if (++Shown >= 12) break;
			}
		}
		return true; // keep ticking — new buckets/foliage tiles appear continuously
	}), 5.0f);
	GWNLContactShadowSweepArmed = true;
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] contact-shadow suppressor armed (belt items + foliage)"));
}

void FWNLPackFixModule::RegisterStaticBaseFix()
{
	// WHY (byte-verified vs UE 5.6.1-CSS): when a movement correction is base-RELATIVE and
	// the client can't resolve the base's net reference, it IGNORES the entire correction
	// (CharacterMovementComponent.cpp:11007) → rubber-banding on factory pieces (~550
	// dropped corrections/session). UFGColoredInstanceMeshProxy bases (instanced rendering
	// proxies on foundations/walls) are exactly that: Movable-mobility for rendering-tech
	// reasons, not net-addressable, yet immobile in gameplay. World-space corrections are
	// applied by clients even with an unresolved base ("WILL use the position!" path).
	//
	// HOOK CHOICE (v0.5.0, hard-learned): v0.3.0 hooked MovementBaseUtility::IsDynamicBase —
	// a function so small that funchook's jump patch corrupted it into trampoline recursion
	// (crash on any world load, incl. the menu scene). Now we hook SendClientAdjustment
	// (large virtual, same proven pattern as our ProcessRemoteFunction gate) and rewrite the
	// server's PendingAdjustment: proxy-based relative corrections become world-space using
	// the server's own resolved base transform. Server-side effect only; NewBase stays set
	// so clients that CAN resolve it keep normal basing.
	// BOUNDARY: only corrections whose base is UFGColoredInstanceMeshProxy. Elevators and
	// vehicles (LinearMotion etc.) use their own components and keep relative basing.
	UCharacterMovementComponent* SampleMove = GetMutableDefault<UCharacterMovementComponent>();
	SUBSCRIBE_METHOD_VIRTUAL(UCharacterMovementComponent::SendClientAdjustment, SampleMove,
		[](auto& Scope, UCharacterMovementComponent* Move)
	{
		if (!Move || !Move->HasValidData())
		{
			return;
		}
		FNetworkPredictionData_Server_Character* Data = Move->GetPredictionData_Server_Character();
		if (!Data)
		{
			return;
		}
		FClientAdjustment& Adj = Data->PendingAdjustment;
		if (Adj.bAckGoodMove || !Adj.bBaseRelativePosition || !Adj.NewBase ||
		    !Adj.NewBase->IsA<UFGColoredInstanceMeshProxy>())
		{
			return;
		}
		FVector WorldLoc;
		if (MovementBaseUtility::TransformLocationToWorld(Adj.NewBase, Adj.NewBaseBoneName, Adj.NewLoc, WorldLoc))
		{
			Adj.NewLoc = WorldLoc;
			Adj.bBaseRelativePosition = false; // client applies world-space even if it can't resolve the proxy
		}
	});
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] static-base movement fix armed (proxy-based corrections -> world-space)"));
}

void FWNLPackFixModule::RegisterRainOcclusionFix()
{
	// WHY: AFGBuildable pieces without an authored mRainOcclusionBoundingBox (IsValid=false)
	// make the rain system log "0 Volume rain occlusion box found!" per registration
	// (thousands/session) AND rain falls straight through them. Ant's call 2026-07-16:
	// fix the data, don't mute the log.
	// TIMING (review finding): buildable classes load LAZILY during save-loading, so a
	// one-shot CDO sweep at world-init misses them. Instead we repair each instance the
	// moment it comes alive — a before-hook on AFGBuildable::BeginPlay (large virtual,
	// the same proven-safe hook class as our RPC gate) runs before the piece registers
	// with the rain subsystem. The CDO is fixed alongside so lightweight (non-actor)
	// instances of the same class inherit the repair too.
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildable::BeginPlay,
		GetMutableDefault<AFGBuildable>(),
		[](auto& Scope, AFGBuildable* Buildable)
	{
		if (!Buildable || !Buildable->DoesAffectOcclusionSystem() ||
		    Buildable->GetOcclusionShape() != EFGRainOcclusionShape::ROCS_Box ||
		    Buildable->mRainOcclusionBoundingBox.IsValid)
		{
			return; // opted out, custom-mesh shape, or properly authored
		}
		const FBox MeshBounds = WNLPackFix_CollectLocalMeshBounds(Buildable->GetClass());
		AFGBuildable* CDO = Cast<AFGBuildable>(Buildable->GetClass()->GetDefaultObject());
		static std::atomic<int32> FixedCount{0};
		static std::atomic<int32> OptedOutCount{0};
		if (MeshBounds.IsValid)
		{
			// Geometry-true occlusion volume — rain now collides where the piece is.
			const FBox3f Box(FVector3f(MeshBounds.Min), FVector3f(MeshBounds.Max));
			Buildable->mRainOcclusionBoundingBox = Box;
			if (CDO) { CDO->mRainOcclusionBoundingBox = Box; }
			const int32 N = ++FixedCount;
			if (N == 1 || (N % 50) == 0)
			{
				UE_LOG(LogWNLPackFix, Display,
					TEXT("[WNLPackFix] rain occlusion fix: %d buildable classes/instances repaired so far (latest: %s)"),
					N, *Buildable->GetClass()->GetName());
			}
		}
		else
		{
			// No visible geometry (snap helpers, markers): the honest answer is that
			// they should never participate in rain occlusion at all.
			Buildable->mAffectsOcclusion = false;
			if (CDO) { CDO->mAffectsOcclusion = false; }
			++OptedOutCount;
		}
	});
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] rain occlusion data fix armed (repairs pieces as they load)"));
}

/** Union of local-space static-mesh bounds: native CDO components + the Blueprint
 *  SimpleConstructionScript templates up the class chain (mod pieces are BPGCs whose
 *  meshes exist only as SCS templates, not on the CDO). */
FBox WNLPackFix_CollectLocalMeshBounds(UClass* Class)
{
	FBox Bounds(ForceInit);
	auto AddComponent = [&Bounds](const UActorComponent* Comp)
	{
		const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(Comp);
		if (SMC && SMC->GetStaticMesh() && !SMC->bHiddenInGame)
		{
			Bounds += SMC->GetStaticMesh()->GetBoundingBox()
				.TransformBy(SMC->GetRelativeTransform());
		}
	};
	if (const AActor* CDO = Cast<AActor>(Class->GetDefaultObject()))
	{
		for (const UActorComponent* Comp : CDO->GetComponents())
		{
			AddComponent(Comp);
		}
	}
	for (UClass* C = Class; C; C = C->GetSuperClass())
	{
		if (const UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(C))
		{
			if (BPGC->SimpleConstructionScript)
			{
				for (const USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
				{
					if (Node && Node->ComponentTemplate)
					{
						AddComponent(Node->ComponentTemplate);
					}
				}
			}
		}
	}
	return Bounds;
}

void FWNLPackFixModule::RegisterStatsSignRpcGate()
{
	// WHY (byte-verified vs UE 5.6.1-CSS NetDriver.cpp:7934): when a remote function is
	// dispatched for an actor with no owning connection, UNetDriver::ProcessRemoteFunction
	// only logs "No owning connection for actor..." and drops the RPC. The Stats mod's sign
	// buildables (Build_StatsSign_C / Build_StatsSign_2_C) hit that path from remote clients
	// ~2.5M times per session (EndingProduction/EndingConsumption fired without ownership),
	// which floods the client's game thread + log — measured as 96% of all client warnings
	// and the dominant "server feels laggy" desync factor. Cancelling the dispatch for
	// exactly this case reproduces vanilla behavior (RPC dropped) minus the cost.
	// BOUNDARY: scoped to the two function names AND the StatsSign class prefix AND the
	// no-owner condition — every other RPC and every other mod's warnings pass untouched.
	static const FName FnEndingProduction(TEXT("EndingProduction"));
	static const FName FnEndingConsumption(TEXT("EndingConsumption"));

	UNetDriver* SampleDriver = GetMutableDefault<UNetDriver>();
	SUBSCRIBE_METHOD_VIRTUAL(UNetDriver::ProcessRemoteFunction, SampleDriver,
		[](auto& Scope, UNetDriver* Driver, AActor* Actor, UFunction* Function,
		   void* Parms, FOutParmRec* OutParms, FFrame* Stack, UObject* SubObject)
	{
		if (!Actor || !Function)
		{
			return;
		}
		// Cheapest test first: two FName compares reject ~all traffic immediately.
		const FName FuncName = Function->GetFName();
		if (FuncName != FnEndingProduction && FuncName != FnEndingConsumption)
		{
			return;
		}
		// Owned actor → legitimate dispatch, engine handles it normally.
		if (Actor->GetNetConnection() != nullptr)
		{
			return;
		}
		// Strictly the Stats mod's signs — other mods keep their diagnostic warnings.
		if (!Actor->GetClass()->GetName().StartsWith(TEXT("Build_StatsSign")))
		{
			return;
		}

		Scope.Cancel(); // vanilla outcome (RPC dropped), without the log write

		static std::atomic<uint64> SuppressedCount{0};
		const uint64 Count = ++SuppressedCount;
		if (Count == 1 || (Count % 100000) == 0)
		{
			// Heartbeat so deployed builds prove the gate is working from the log alone.
			UE_LOG(LogWNLPackFix, Display,
				TEXT("[WNLPackFix] StatsSign RPC gate: suppressed %llu no-owner dispatches this session"), Count);
		}
	});

	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] StatsSign RPC gate armed"));
}

void FWNLPackFixModule::RegisterWwiseServerAudioGate()
{
	// SERVER-ONLY. A dedicated server has no audio device, so UAkGameplayStatics::StopActor
	// always fails FAkAudioDevice::Get(), logs "Could not retrieve audio device." and returns
	// having stopped nothing (verified AkGameplayStatics.cpp:974-979). A per-actor FG cleanup
	// path calls it constantly, flooding LogAkAudio (~3164 warnings/session on the server log).
	// We arm the hook ONLY on the dedicated server — there the original is a guaranteed no-op, so
	// cancelling it is behaviour-identical minus the log write. Clients skip registration entirely:
	// their audio device is real and StopActor must run to actually stop sounds.
	// BOUNDARY: one function, dedicated-server only, unconditional cancel — no client path touched,
	// no other Wwise call affected.
	if (!IsRunningDedicatedServer())
	{
		return; // clients keep working audio; nothing to gate
	}

	SUBSCRIBE_METHOD(UAkGameplayStatics::StopActor, [](auto& Scope, AActor* /*Actor*/)
	{
		Scope.Cancel(); // server device is always null → the original would only log + return

		static std::atomic<uint64> SuppressedCount{0};
		const uint64 Count = ++SuppressedCount;
		if (Count == 1 || (Count % 100000) == 0)
		{
			// Heartbeat so a deployed server proves the gate is live from its log alone.
			UE_LOG(LogWNLPackFix, Display,
				TEXT("[WNLPackFix] Wwise server audio gate: suppressed %llu StopActor no-ops this session"), Count);
		}
	});

	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] Wwise server audio gate armed (dedicated server)"));
}

void FWNLPackFixModule::RegisterNavMeshCoverageFix()
{
	// PROBLEM (server-side, verified from engine source): CSS caps ARecastNavMesh::TileNumberHardLimit
	// at 65536 (Config/DefaultEngine.ini:374). Our large modded map needs 306,440 tiles for full
	// bounds, so RecastNavMeshGenerator.cpp:5542 clamps it to 65536 -> only ~21% of the map has
	// navmesh -> creatures cannot path across the other ~79%.
	//
	// WHY RAISING IT IS MEMORY-SAFE HERE:
	//  * Only the empty tile SLOT table is allocated up front, at 176 bytes/tile (engine's own
	//    comment, RecastNavMesh.h:747). At full coverage that's ~306,440 * 176B ~= 51 MB per navmesh
	//    actor (up from ~11 MB at 65536). The array tracks the map's REAL tile need, not the ceiling,
	//    so a generous ceiling does not over-allocate.
	//  * The heavy per-tile geometry is STREAMED by World Partition (bIsWorldPartitioned=true) - it is
	//    sized by the currently-resident/active area around nav-invokers, NOT by this ceiling. Raising
	//    the ceiling does not pull the whole map's geometry into RAM.
	//  * RuntimeGeneration=Dynamic (DefaultEngine.ini:353): previously-blocked tiles generate
	//    incrementally as creatures reach them - ordinary async tile-gen, no one-time rebuild stall.
	//  * dtPolyRef is 64-bit (USE_64BIT_ADDRESS not overridden), so a 2^19 ceiling leaves the
	//    poly-per-tile bit budget untouched (starvation only past ~134M tiles).
	//
	// Runs on ALL non-editor targets (no NetMode at StartupModule): a CDO default is inert wherever no
	// navmesh generator runs, so a client that builds no navmesh pays nothing; the server (and SP/listen)
	// picks it up. The ceiling is read live off the actor at generator-construct each session, so setting
	// the class-default now applies on the next world load. TileNumberHardLimit is a `config` UPROPERTY;
	// we set it by reflection on each FG navmesh subclass CDO (robust to its access level). We do NOT
	// touch TileSizeUU (changing it would invalidate the tile grid -> full rebuild + coarser paths).
	// CAVEAT (review finding): if a placed navmesh actor in the level carries its OWN serialized
	// TileNumberHardLimit, that instance value beats the CDO and this no-ops. BOOT-VERIFY the tile count
	// in the nav log; if still clamped ~65536, add a per-instance write-back (mirror RegisterRainOcclusionFix).
	static constexpr int32 NavMeshTileHardLimit = 524288; // 2^19, clears the 306,440-tile request with headroom

	// Every AFGNavMeshBase subclass the game actually spawns gets its own m_tiles array, so raise each.
	// Access is DIRECT (compile-checked): Config/AccessTransformers.ini friends this module class into
	// ARecastNavMesh — the docs-recommended pattern, replacing the old FProperty-by-name reflection
	// (which would only fail at RUNTIME if CSS ever renamed the field).
	UClass* NavClasses[] = {
		AFGDefaultNavMesh::StaticClass(), AFGCritterNavMesh::StaticClass(), AFGAlphaNavMesh::StaticClass(),
		AFGEliteNavMesh::StaticClass(),   AFGGiraffeNavMesh::StaticClass(), AFGTurtleNavMesh::StaticClass(),
	};

	int32 Raised = 0;
	for (UClass* NavClass : NavClasses)
	{
		if (!NavClass) { continue; }
		ARecastNavMesh* CDO = Cast<ARecastNavMesh>(NavClass->GetDefaultObject());
		if (!CDO) { continue; }
		const int32 Old = CDO->TileNumberHardLimit;
		if (Old < NavMeshTileHardLimit) // never lower a value the user/config set higher
		{
			CDO->TileNumberHardLimit = NavMeshTileHardLimit;
			++Raised;
			UE_LOG(LogWNLPackFix, Display,
				TEXT("[WNLPackFix]   %s: TileNumberHardLimit %d -> %d"), *NavClass->GetName(), Old, NavMeshTileHardLimit);
		}
	}

	// ~51 MB slot table per actor at full coverage vs ~11 MB before; geometry stays WP-streamed.
	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] navmesh coverage fix: raised %d nav classes to %d-tile ceiling ")
		TEXT("(full-map creature pathing; ~+40 MB slot table per active nav class, geometry stays streamed)"),
		Raised, NavMeshTileHardLimit);
}

IMPLEMENT_MODULE(FWNLPackFixModule, WNLPackFix);
