// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMDistanceFieldAudit.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

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
	TEXT("Re-enable distance-field contribution on instanced meshes that are missing it. "
	     "0 = audit only (default), 1 = repair and report the count. This ADDS renderer work - read the "
	     "audit line first and decide whether the count justifies it."),
	ECVF_Default);

namespace
{
	/*
	 * When to look. AbstractInstanceManager re-enables distance fields on the one tick where both lazy
	 * queues drain (AbstractInstanceManager.cpp:482-498), and that is somewhere after the loading screen
	 * rather than at a fixed moment. So the audit samples at spreading intervals instead of guessing one,
	 * and the DIRECTION between samples is the finding: FALLING means the re-enable pass is still
	 * working, UNCHANGED means these were missed, RISING means the deficit is growing as you play.
	 *
	 * ⚠ EXTENDED FROM THREE MARKS TO FIVE ON 2026-08-10, because three could not tell a LAG from a LEAK.
	 *
	 * At 10/30/90 s the count was still rising on Ant's save, and a rising count at 90 s has two
	 * completely different readings: vanilla has not caught up yet (harmless, wait), or the deficit
	 * grows for as long as you play (needs fixing). Only a much later sample separates them, so the
	 * schedule now runs out to 15 minutes. The cost is two more audits per session.
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

		const double TAnalyseStart = FPlatformTime::Seconds();
		C.GatherMs = (TAnalyseStart - TGatherStart) * 1000.0;

		/** Per-worker workspace. Mutated without synchronisation because each task owns one. */
		struct FDfCtx
		{
			int32 Components = 0;
			int32 Missing = 0;
			int64 MissingInstances = 0;
			TArray<TPair<int32, UInstancedStaticMeshComponent*>> Offenders;
		};

		TArray<FDfCtx> Contexts;
		ParallelForWithTaskContext(Contexts, AllComps.Num(),
			[&AllComps](FDfCtx& Ctx, int32 Index)
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

				// ⚠ POINTERS ONLY. Resolving GetStaticMesh()->GetName() would build an FString per
				// offender on a worker; the names are wanted for at most five log lines, so they are
				// resolved on the game thread below where that is unambiguously safe.
				if (Ctx.Offenders.Num() < 64)
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
		if (bRepair)
		{
			for (UInstancedStaticMeshComponent* Comp : AllComps)
			{
				if (Comp == nullptr || Comp->bAffectDistanceFieldLighting) { continue; }
				Comp->bAffectDistanceFieldLighting = true;
				Comp->MarkRenderStateDirty();
				++C.Repaired;
			}
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

		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] distance-field audit (%s): %d of %d instanced mesh component(s) do NOT contribute "
			     "to distance fields, carrying %lld instance(s) between them. AbstractInstance switches "
			     "this off during lazy load (AbstractInstanceManager.cpp:305) and re-enables it in a "
			     "one-shot pass when the queues drain (:482-498). Anything still off has missed that pass "
			     "and is invisible to every distance-field consumer in the renderer."),
			When, C.Missing, C.Components, C.MissingInstances);

		for (const FString& S : Worst)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM]   worst: %s"), *S);
		}

		ReportCost(C);

		if (C.Repaired > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   REPAIRED %d component(s) - they now contribute. ⚠ That ADDS renderer work. "
				     "Measure the frame cost before leaving FPM.DistanceField.Repair on."), C.Repaired);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   audit only. FPM.DistanceField.Repair 1 fixes them, and costs renderer work "
				     "in exchange - the count above is how much."));
		}

		FPMOverlay::Post(TEXT("distance-field"),
			FString::Printf(TEXT("%d/%d components missing DF (%lld instances)"),
				C.Missing, C.Components, C.MissingInstances));
	}
}

FFPMDistanceFieldAudit& FFPMDistanceFieldAudit::Get()
{
	static FFPMDistanceFieldAudit Instance;
	return Instance;
}

void FFPMDistanceFieldAudit::Arm()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] distance-field audit ARMED - READ ONLY by default, no hook. It samples at %.0f/%.0f/%.0f s "
		     "after each world load and counts instanced mesh components that are NOT contributing to "
		     "distance fields. One suspected cause under three symptoms: rain through walls (NS_Rain owns "
		     "a QueryMeshDistanceFieldGPU, so if the wall is not IN the field the query is innocent), "
		     "light through terrain on low settings, and the parked DF shadow pop-in."),
		GFPMDfSamplesSec[0], GFPMDfSamplesSec[1], GFPMDfSamplesSec[2]);
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
				 * ★ THE DIRECTION IS THE FINDING, AND THIS TESTED ONLY FOR CHANGE UNTIL 2026-08-10.
				 *
				 * The old branch was `C.Missing != GFPMDfLastMissing` and printed "vanilla's re-enable
				 * pass is still working through the world" for ANY movement. The comment above it already
				 * knew better — "a count that FALLS between samples means the re-enable pass is still
				 * working" — and the code did not check which way it went.
				 *
				 * Measured on Ant's save that evening: 19769 -> 19996 components, 1028518 -> 1042346
				 * instances. **It rose.** The old line told her vanilla was fixing it while the deficit
				 * grew, which is worse than saying nothing: it is a diagnostic arguing against its own
				 * data.
				 *
				 * Three outcomes now, because there are three and they mean opposite things:
				 *   FELL      - vanilla's one-shot re-enable pass is draining the backlog. Wait.
				 *   ROSE      - the world is adding un-contributing instances faster than that pass fixes
				 *               them. This is the one that needs FPM, and it is what she measured.
				 *   UNCHANGED - the pass has finished and these were missed. Also needs FPM.
				 */
				if (GFPMDfLastMissing >= 0 && C.Missing < GFPMDfLastMissing)
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM]   missing count FELL %d -> %d since the last sample: vanilla's "
						     "re-enable pass is still working through the world. Not a finding yet."),
						GFPMDfLastMissing, C.Missing);
				}
				else if (GFPMDfLastMissing >= 0 && C.Missing > GFPMDfLastMissing)
				{
					UE_LOG(LogFicsitsPerformanceManager, Warning,
						TEXT("[FPM]   ⚠ missing count ROSE %d -> %d (+%d) since the last sample. The world "
						     "is producing instances that do not contribute to distance fields FASTER than "
						     "vanilla's re-enable pass repairs them, so this deficit GROWS as you play. A "
						     "one-shot repair cannot win that race."),
						GFPMDfLastMissing, C.Missing, C.Missing - GFPMDfLastMissing);
				}
				else if (GFPMDfLastMissing >= 0 && C.Missing > 0)
				{
					UE_LOG(LogFicsitsPerformanceManager, Warning,
						TEXT("[FPM]   missing count UNCHANGED at %d since the last sample. These are not "
						     "waiting on lazy load - they were missed."), C.Missing);
				}
			}

			GFPMDfLastMissing = C.Missing;
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

void FFPMDistanceFieldAudit::AuditNow()
{
	UWorld* World = GFPMDfWorld.Get();
	if (World == nullptr && GEngine != nullptr) { World = GEngine->GetCurrentPlayWorld(); }

	TArray<FString> Worst;
	const FFPMDfCount C = CountAndMaybeRepair(World, CVarDistanceFieldRepair.GetValueOnGameThread() != 0, Worst);
	Report(C, Worst, TEXT("on demand"));
}

static FAutoConsoleCommand GFPMDfAuditCmd(
	TEXT("FPM.DistanceField.Audit"),
	TEXT("Count instanced mesh components that are not contributing to distance fields, and name the "
	     "worst by instance count."),
	FConsoleCommandDelegate::CreateStatic(&FFPMDistanceFieldAudit::AuditNow));
