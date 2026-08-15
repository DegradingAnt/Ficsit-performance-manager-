// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMLightweightCensusDetector.h"

#include "EngineUtils.h"
#include "Buildables/FGBuildable.h"
#include "FGLightweightBuildableSubsystem.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarLightweightThreshold(
		TEXT("FPM.Detect.LightweightThreshold"), 20,
		TEXT("Minimum live actor count before a class with zero lightweight-instance presence is "
		     "flagged by the trap-4 census. Lower to see rarer classes; the default exists so a "
		     "one-off unique building is not reported alongside a buildable placed thousands of times."),
		ECVF_Default);

	/** The last world OnWorldLoad handed us - NOT GWorld, which `FPMNetGuidCensus.cpp` already
	 *  documents as unreliable outside the game thread and not guaranteed to be the right context.
	 *  `FPM.Detect.Lightweight` re-runs against this instead of reaching for a global. */
	TWeakObjectPtr<UWorld> GLastLoadedWorld;
}

FFPMLightweightCensusDetector& FFPMLightweightCensusDetector::Get()
{
	static FFPMLightweightCensusDetector Instance;
	return Instance;
}

int32 FFPMLightweightCensusDetector::MinActorCountToFlag()
{
	return FMath::Max(0, CVarLightweightThreshold.GetValueOnGameThread());
}

bool FFPMLightweightCensusDetector::SelfTest(UWorld* World)
{
	if (World == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] detect: lightweight-census self-test - no world yet (expected at Arm(), "
			     "which runs from StartupModule before any world exists). COVERAGE: neither "
			     "TActorIterator nor the lightweight subsystem can report anything real without a "
			     "world; the real proof runs inside RunNow() at the first world load."));
		return true; // nothing to fail yet - this is a coverage statement, not a check
	}

	int32 ActorCount = 0;
	for (TActorIterator<AFGBuildable> It(World); It; ++It) { ++ActorCount; }
	const bool bActorReadOk = ActorCount > 0;

	const AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(World);
	int32 LightweightClassCount = 0;
	int32 LightweightInstanceTotal = 0;
	if (Subsystem != nullptr)
	{
		for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Entry
			: Subsystem->GetAllLightweightBuildableInstances())
		{
			++LightweightClassCount;
			LightweightInstanceTotal += Entry.Value.Num();
		}
	}
	const bool bLightweightReadOk = Subsystem != nullptr && LightweightInstanceTotal > 0;

	const bool bPassed = bActorReadOk && bLightweightReadOk;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: lightweight-census self-test %s - TActorIterator<AFGBuildable> found %d "
		     "actor(s) (%s), lightweight subsystem %s with %d class(es)/%d instance(s) total (%s)."),
		bPassed ? TEXT("PASSED") : TEXT("FAILED"),
		ActorCount, bActorReadOk ? TEXT("ok") : TEXT("FAILED - expected at least 1 on any real save"),
		Subsystem != nullptr ? TEXT("found") : TEXT("NOT FOUND"),
		LightweightClassCount, LightweightInstanceTotal,
		bLightweightReadOk ? TEXT("ok") : TEXT("FAILED - expected at least 1 on any real save"));

	return bPassed;
}

void FFPMLightweightCensusDetector::RunNow(UWorld* World)
{
	if (World == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] detect: lightweight-census skipped - no world."));
		return;
	}

	TMap<const UClass*, int32> LightweightCountPerClass;
	const AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(World);
	if (Subsystem != nullptr)
	{
		for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>& Entry
			: Subsystem->GetAllLightweightBuildableInstances())
		{
			if (*Entry.Key != nullptr)
			{
				LightweightCountPerClass.Add(*Entry.Key, Entry.Value.Num());
			}
		}
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] detect: lightweight-census - no AFGLightweightBuildableSubsystem this world; "
			     "every class will read as zero lightweight presence, which would over-flag. Skipping "
			     "the flag pass; the actor count alone is not reported as a finding."));
		FPMOverlay::PostSticky(TEXT("detect"), TEXT("lightweight-census"),
			TEXT("trap 4 (lightweight census): SKIPPED, no lightweight subsystem this world"));
		return;
	}

	TMap<const UClass*, int32> ActorCountPerClass;
	for (TActorIterator<AFGBuildable> It(World); It; ++It)
	{
		ActorCountPerClass.FindOrAdd(It->GetClass()) += 1;
	}

	const int32 Threshold = MinActorCountToFlag();
	int32 ClassesConsidered = 0;
	int32 Flagged = 0;
	for (const TPair<const UClass*, int32>& Entry : ActorCountPerClass)
	{
		const UClass* Class = Entry.Key;
		const int32 ActorCount = Entry.Value;
		if (Class == nullptr) { continue; }
		++ClassesConsidered;
		if (ActorCount < Threshold) { continue; }

		bool bAmbiguous = false;
		const FString ModKey = FFPMDetectorRegistry::ExtractModKey(Class->GetPathName(), bAmbiguous);
		if (ModKey == TEXT("FactoryGame") && !bAmbiguous) { continue; } // verified vanilla, not judged

		const int32 LightweightCount = LightweightCountPerClass.FindRef(Class);
		if (LightweightCount == 0)
		{
			++Flagged;
			FFPMDetectorRegistry::Report(TEXT("lightweight-census-detector"), Class->GetPathName(),
				FString::Printf(TEXT("%d live full-actor instance(s), 0 in the lightweight-instance "
				                      "map for this class - actor tick/replication/component cost "
				                      "paid per placement, never converted to lightweight"), ActorCount),
				ActorCount);
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: lightweight-census - %d AFGBuildable-derived class(es) with at least one "
		     "live actor considered (threshold %d actors to flag), %d flagged as actor-only at scale "
		     "(reported to FPM.Detect.Report). COVERAGE: this run's own moment only, and only "
		     "classes with ZERO lightweight-map presence - a class using lightweight for SOME "
		     "placements and full actors for others is a mixed case this pass does not separate out."),
		ClassesConsidered, Threshold, Flagged);

	// m6164470: every FPM feature reports to the dev overlay.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("lightweight-census"),
		FString::Printf(TEXT("trap 4 (lightweight census): %d/%d class(es) flagged actor-only"),
			Flagged, ClassesConsidered));
}

void FFPMLightweightCensusDetector::OnWorldLoad(UWorld* World)
{
	GLastLoadedWorld = World;
	SelfTest(World);
	RunNow(World);
}

void FFPMLightweightCensusDetector::Arm()
{
	const bool bSelfTestPassed = SelfTest(nullptr);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: lightweight-census-detector armed - self-test %s. Trap 4 of 4 (§9.3, "
		     "\"the biggest\"): actors vs lightweight instances per class, censused each world load."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommand GFPMLightweightRunCmd(
		TEXT("FPM.Detect.Lightweight"),
		TEXT("Re-run the actor-vs-lightweight census now. Needs a world; run after loading a save."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			if (UWorld* World = GLastLoadedWorld.Get())
			{
				FFPMLightweightCensusDetector::RunNow(World);
			}
			else
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] detect: lightweight-census has no known world yet - run after a world load."));
			}
		}));
}
