// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMDistanceFieldAudit.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

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
	 * rather than at a fixed moment.
	 *
	 * So the audit samples at three spreading intervals instead of guessing one. A count that FALLS
	 * between samples means the re-enable pass is still working through the world and the early reading
	 * was premature. A count that stays put is the finding.
	 */
	constexpr float GFPMDfSamplesSec[] = { 10.f, 30.f, 90.f };
	int32 GFPMDfSampleIndex = 0;
	float GFPMDfElapsed = 0.f;
	FTSTicker::FDelegateHandle GFPMDfTicker;
	TWeakObjectPtr<UWorld> GFPMDfWorld;

	int32 GFPMDfLastMissing = -1;

	struct FFPMDfCount
	{
		int32 Components = 0;
		int32 Missing = 0;         // bAffectDistanceFieldLighting == false
		int32 MissingInstances = 0; // instance count carried by those components — the visible weight
		int32 Repaired = 0;
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
		TArray<TPair<int32, FString>> Offenders;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UInstancedStaticMeshComponent*> Comps;
			It->GetComponents<UInstancedStaticMeshComponent>(Comps);

			for (UInstancedStaticMeshComponent* Comp : Comps)
			{
				if (Comp == nullptr) { continue; }
				++C.Components;

				if (Comp->bAffectDistanceFieldLighting) { continue; }

				const int32 Instances = Comp->GetInstanceCount();
				++C.Missing;
				C.MissingInstances += Instances;

				if (Offenders.Num() < 64)
				{
					Offenders.Emplace(Instances, Comp->GetStaticMesh()
						? Comp->GetStaticMesh()->GetName()
						: FString(TEXT("<no mesh>")));
				}

				if (bRepair)
				{
					Comp->bAffectDistanceFieldLighting = true;
					Comp->MarkRenderStateDirty();
					++C.Repaired;
				}
			}
		}

		// Name the biggest offenders by INSTANCE count, not by component count — one component holding
		// 4,000 foundations matters more than forty holding one pipe each.
		Offenders.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B)
			{ return A.Key > B.Key; });
		for (int32 i = 0; i < FMath::Min(5, Offenders.Num()); ++i)
		{
			OutWorstNames.Add(FString::Printf(TEXT("%s x%d"), *Offenders[i].Value, Offenders[i].Key));
		}

		return C;
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
			return;
		}

		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] distance-field audit (%s): %d of %d instanced mesh component(s) do NOT contribute "
			     "to distance fields, carrying %d instance(s) between them. AbstractInstance switches this "
			     "off during lazy load (AbstractInstanceManager.cpp:305) and re-enables it in a one-shot "
			     "pass when the queues drain (:482-498). Anything still off has missed that pass and is "
			     "invisible to every distance-field consumer in the renderer."),
			When, C.Missing, C.Components, C.MissingInstances);

		for (const FString& S : Worst)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM]   worst: %s"), *S);
		}

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
			FString::Printf(TEXT("%d/%d components missing DF (%d instances)"),
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
			UWorld* W = GFPMDfWorld.Get();
			if (W == nullptr) { return false; }

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
				const TCHAR* When = TEXT("late");
				if (GFPMDfSampleIndex == 0) { When = TEXT("early"); }
				else if (GFPMDfSampleIndex == 1) { When = TEXT("mid"); }

				Report(C, Worst, When);

				if (GFPMDfLastMissing >= 0 && C.Missing != GFPMDfLastMissing)
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM]   missing count moved %d -> %d since the last sample: vanilla's "
						     "re-enable pass is still working through the world."),
						GFPMDfLastMissing, C.Missing);
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
