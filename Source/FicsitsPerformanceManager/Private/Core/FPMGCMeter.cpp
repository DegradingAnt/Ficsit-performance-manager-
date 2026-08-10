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
	/**
	 * ★ PASSES THAT RAN DURING ASYNC LOADING -- and this looks like a dead counter until you read WHICH
	 * path carries the gate.
	 *
	 * `UnrealEngine.cpp:2017` gates the TIMER-driven pass on
	 * `if (GPerformGCWhileAsyncLoading || !IsAsyncLoading())`, and that flag defaults to 0 (`:1664`). So
	 * a timer pass can never land during a stream, and this would be structurally zero if timer passes
	 * were the only kind.
	 *
	 * They are not. A FORCED `CollectGarbage()` does not go through that gate. So a non-zero here means
	 * a forced stop-the-world pause landed ON TOP OF a streaming stall -- which is exactly the shape of
	 * Ant's "hitches moving fast through the world", and is worth far more than the count suggests.
	 */
	int32 GFPMGCDuringAsyncLoad = 0;
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

	/**
	 * ★ CLAIMED object count -- NOT `GetObjectArrayNum()`, and the difference is the whole measurement.
	 *
	 * Caught by verifying the API instead of trusting the compile. `GetObjectArrayNum()` says of itself
	 * *"Returns the size of the global UObject array, some of these might be unused"* (UObjectArray.h:1174)
	 * -- it is `ObjObjects.Num()`, the ARRAY SIZE, which includes freed slots. A before/after delta taken
	 * from it can read ZERO across a pass that collected thousands of objects, because the array does not
	 * shrink. That is a dead instrument: a confident number that cannot move.
	 *
	 * `GetObjectArrayNumMinusAvailable()` (:1209) is `ObjObjects.Num() - ObjAvailableList.Num()` --
	 * "the number of objects claimed". That is what a collection actually changes.
	 */
	int32 LiveObjects()
	{
		return GUObjectArray.GetObjectArrayNumMinusAvailable();
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
				TEXT("[FPM] GC #%d: pause %.1f ms | %.1f s since last (target %.1f, %s) | claimed objects "
				     "%d -> %d (%+d) | asyncloading=%d"),
				GFPMGCPasses, PauseMs, SinceLast, Target,
				SinceLast < 0.0 ? TEXT("FIRST") : (bForced ? TEXT("FORCED") : TEXT("timer")),
				GFPMGCObjectsBefore, ObjectsAfter, ObjectsAfter - GFPMGCObjectsBefore, bAsync ? 1 : 0);
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

/*
 * ★ THE DELEGATES DEMONSTRABLY FIRE IN A SHIPPING BUILD, verified from engine bytes rather than assumed:
 *
 *     GarbageCollection.cpp:5531   GetPreGarbageCollectDelegate().Broadcast();
 *     GarbageCollection.cpp:5733   GetPostGarbageCollect().Broadcast();
 *
 * Neither sits behind `#if WITH_EDITOR`. The pre-side broadcast is unconditional.
 *
 * ⚠ THE POST SIDE HAS ONE GUARD, AND IT IS THE ONE THING THAT COULD KILL THIS INSTRUMENT:
 *     GarbageCollection.cpp:5729   if (!GIsIncrementalReachabilityPending)
 * With incremental reachability enabled, a pass can end without broadcasting -- the meter would then
 * count a pre with no post and silently under-report. It is BANNED on this build (setting
 * gc.AllowIncrementalReachability 1 crashed Ant's save in 34 s, and FPM2 ships no switch for it), so
 * the guard is satisfied today. Recorded because it is a dependency, not a certainty: if that ban is
 * ever lifted, this meter needs a pre-without-post counter before it can be trusted again.
 */
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
		     "This build reports gc.AllowParallelGC=%s, gc.TimeBetweenPurgingPendingKillObjects=%s. "
		     "It steers NOTHING - no quality lever shortens a mark that scales with the live object "
		     "graph."),
		Parallel ? *Parallel->GetString() : TEXT("<not found>"),
		Interval ? *Interval->GetString() : TEXT("<not found>"));

	/*
	 * ⚠ THE ENGINE'S EFFECTIVE INTERVAL IS DELIBERATELY NOT PRINTED HERE.
	 *
	 * The first version printed it at arm time and it read "0.0 s" on the 2026-08-10 boot, because
	 * StartupModule runs before GEngine is usable. A zero from an instrument that cannot yet read is
	 * worse than no line at all - it invites exactly the wrong conclusion about the collection cadence.
	 * Every per-pass line carries the live target, which is where it is meaningful anyway.
	 */
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
		TEXT("[FPM] gc-meter: %d pass(es) - mean %.1f ms, worst %.1f ms (%.0f s ago), %.1f ms total. "
		     "%d claimed object(s)."),
		GFPMGCPasses, GFPMGCTotalMs / GFPMGCPasses, GFPMGCWorstMs,
		GFPMGCWorstAtSeconds > 0.0 ? FPlatformTime::Seconds() - GFPMGCWorstAtSeconds : -1.0,
		GFPMGCTotalMs, LiveObjects());

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
		TEXT("[FPM]   %d pass(es) ran DURING async loading. The TIMER path cannot do this - it is gated on "
		     "!IsAsyncLoading() (UnrealEngine.cpp:2017, GPerformGCWhileAsyncLoading defaults 0 at :1664) - "
		     "so every one of these was a FORCED collection landing on top of a streaming stall. That is "
		     "the 'hitches moving fast through the world' shape, and it is the highest-value lead here."),
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
