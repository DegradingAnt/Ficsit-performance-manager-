// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMGCMeter.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	double GFPMGCPreTime = 0.0;
	double GFPMGCLastPassTime = 0.0;
	int32 GFPMGCObjectsBefore = 0;

	int32 GFPMGCPasses = 0;
	int32 GFPMGCForced = 0;          // arrived well before the timer was due
	int32 GFPMGCTimer = 0;           // arrived at or after the timer
	int32 GFPMGCDuringAsyncLoad = 0; // the engine normally skips GC while async loading
	double GFPMGCTotalMs = 0.0;
	double GFPMGCWorstMs = 0.0;
	double GFPMGCWorstAtSeconds = 0.0;

	FDelegateHandle GFPMGCPreHandle;
	FDelegateHandle GFPMGCPostHandle;

	/**
	 * A pass counts as FORCED when it arrives noticeably before the timer was due.
	 *
	 * ⚠ THIS IS A CLASSIFIER, NOT A READING. The engine does not tell us who asked; the forcing call
	 * sites are invisible from here because the SML stubs have empty bodies. So the split is INFERRED
	 * from timing and the log says so. 0.9 leaves room for a pass that fires a fraction early on a
	 * frame boundary without being labelled forced.
	 */
	constexpr double GFPMGCForcedFraction = 0.9;

	/** Live UObject count. Cheap: one array size read. */
	int32 LiveObjects()
	{
		return GUObjectArray.GetObjectArrayNum();
	}

	float TargetInterval()
	{
		return GEngine ? GEngine->GetTimeBetweenGarbageCollectionPasses() : 0.f;
	}

	void OnPreGC()
	{
		GFPMGCPreTime = FPlatformTime::Seconds();
		GFPMGCObjectsBefore = LiveObjects();
	}

	void OnPostGC()
	{
		const double Now = FPlatformTime::Seconds();
		if (GFPMGCPreTime <= 0.0)
		{
			// Post without a pre. Not a finding about GC; the meter armed mid-pass.
			return;
		}

		const double PauseMs = (Now - GFPMGCPreTime) * 1000.0;
		const double SinceLast = (GFPMGCLastPassTime > 0.0) ? (Now - GFPMGCLastPassTime) : -1.0;
		const float Target = TargetInterval();
		const int32 ObjectsAfter = LiveObjects();
		const bool bAsync = IsAsyncLoading();

		++GFPMGCPasses;
		GFPMGCTotalMs += PauseMs;
		if (PauseMs > GFPMGCWorstMs)
		{
			GFPMGCWorstMs = PauseMs;
			GFPMGCWorstAtSeconds = Now;
		}
		if (bAsync) { ++GFPMGCDuringAsyncLoad; }

		/*
		 * ★ THE SPLIT THIS METER EXISTS FOR. The one pacing lever available only stretches the TIMER
		 * passes; forced ones are untouched. So the ratio decides whether that lever is worth writing at
		 * all, and the design's 22-vs-27 arithmetic that predicted "about five forced" is a hypothesis
		 * this line either confirms or kills.
		 */
		const bool bForced = (SinceLast >= 0.0) && (Target > 0.f)
			&& (SinceLast < Target * GFPMGCForcedFraction);
		if (SinceLast >= 0.0) { bForced ? ++GFPMGCForced : ++GFPMGCTimer; }

		GFPMGCLastPassTime = Now;
		GFPMGCPreTime = 0.0;

		if (FPMDiag::IsOn(FPMDiag::EChannel::GCMeter))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] GC #%d: pause %.1f ms | %.1f s since last (target %.1f, %s) | objects %d -> %d "
				     "| asyncloading=%d"),
				GFPMGCPasses, PauseMs, SinceLast, Target,
				SinceLast < 0.0 ? TEXT("FIRST") : (bForced ? TEXT("FORCED") : TEXT("timer")),
				GFPMGCObjectsBefore, ObjectsAfter, bAsync ? 1 : 0);
		}

		// A pause this long is a visible stutter, so it is worth the overlay even at low verbosity.
		if (PauseMs >= 50.0)
		{
			FPMOverlay::Post(TEXT("gc"), FString::Printf(TEXT("%.0f ms pause (#%d)"), PauseMs, GFPMGCPasses));
		}
	}
}

FFPMGCMeter& FFPMGCMeter::Get()
{
	static FFPMGCMeter Instance;
	return Instance;
}

void FFPMGCMeter::Arm()
{
	GFPMGCPreHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddStatic(&OnPreGC);
	GFPMGCPostHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(&OnPostGC);

	/*
	 * ARM-TIME SANITY RECEIPTS. The design closed two surfaces by asserting engine defaults — parallel
	 * mark already on, the purge timer at 60 s. Printing what they ACTUALLY are on this machine costs
	 * two console reads and turns both from "the source says" into "this build says". Another mod may
	 * also be in the pot, and this is where that shows.
	 */
	const IConsoleVariable* Parallel = IConsoleManager::Get().FindConsoleVariable(TEXT("gc.AllowParallelGC"));
	const IConsoleVariable* Interval =
		IConsoleManager::Get().FindConsoleVariable(TEXT("gc.TimeBetweenPurgingPendingKillObjects"));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] gc-meter ARMED - READ ONLY, no hook, two engine delegates. Baseline to beat, measured "
		     "2026-08-02 on Ant's save: 27 pauses in ~22 min, mean 27.2 ms, worst 148.6 ms. "
		     "This build reports gc.AllowParallelGC=%s, gc.TimeBetweenPurgingPendingKillObjects=%s, "
		     "engine target interval %.1f s. It steers NOTHING - no quality lever shortens a mark that "
		     "scales with the live object graph."),
		Parallel ? *Parallel->GetString() : TEXT("<not found>"),
		Interval ? *Interval->GetString() : TEXT("<not found>"),
		TargetInterval());
}

void FFPMGCMeter::Disarm()
{
	if (GFPMGCPreHandle.IsValid())
	{
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(GFPMGCPreHandle);
		GFPMGCPreHandle.Reset();
	}
	if (GFPMGCPostHandle.IsValid())
	{
		FCoreUObjectDelegates::GetPostGarbageCollect().Remove(GFPMGCPostHandle);
		GFPMGCPostHandle.Reset();
	}
}

void FFPMGCMeter::ReportNow()
{
	if (GFPMGCPasses == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] gc-meter: NO PASSES SEEN. Either the session is younger than the collection "
			     "interval (%.1f s), or the delegates did not bind. That is a statement about the "
			     "instrument, not about garbage collection."), TargetInterval());
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] gc-meter: %d pass(es) - mean %.1f ms, worst %.1f ms, %.1f ms total. %d live object(s)."),
		GFPMGCPasses, GFPMGCTotalMs / GFPMGCPasses, GFPMGCWorstMs, GFPMGCTotalMs, LiveObjects());

	/*
	 * ★ THE LINE THAT DECIDES THE NEXT FIX. Raising the purge interval only stretches TIMER passes. If
	 * the forced share dominates, that lever is close to worthless here and the follow-up has to find
	 * what is forcing them instead.
	 */
	const int32 Classified = GFPMGCForced + GFPMGCTimer;
	if (Classified > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %d timer-driven, %d FORCED (%.0f%% forced). The only pacing lever available "
			     "(gc.TimeBetweenPurgingPendingKillObjects) stretches ONLY the timer passes - so at this "
			     "ratio it would remove about %d of the %d."),
			GFPMGCTimer, GFPMGCForced, 100.0 * GFPMGCForced / Classified,
			GFPMGCTimer / 2, Classified);
	}

	UE_CLOG(GFPMGCDuringAsyncLoad > 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   %d pass(es) ran DURING async loading. The engine normally skips GC while async "
		     "loading, so this is unexpected and worth chasing - it would put a stop-the-world pause on "
		     "top of a streaming stall, which is exactly the 'hitches moving fast' shape."),
		GFPMGCDuringAsyncLoad);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ⚠ the timer/forced split is a CLASSIFIER, not a reading: the engine does not say "
		     "who asked, so it is inferred from arrival time against the %.1f s target."), TargetInterval());
}

static FAutoConsoleCommand GFPMGCReportCmd(
	TEXT("FPM.GC.Report"),
	TEXT("Garbage collection: pass count, mean and worst pause, and the timer-vs-forced split that "
	     "decides whether the pacing lever is worth writing."),
	FConsoleCommandDelegate::CreateStatic(&FFPMGCMeter::ReportNow));
