// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMGCMeter.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
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

	/**
	 * ★★ THE FULL-PURGE SPLIT -- and unlike the timer/forced one above, this is a READING, not a guess.
	 *
	 * A full purge and an ordinary pass are not two samples of one population. They differ by four whole
	 * phases, every one of them inside the pre/post window this meter times
	 * (Engine/Source/Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp):
	 *   :5698  `if (bPerformFullPurge || !GIncrementalBeginDestroyEnabled)` -> UnhashUnreachableObjects,
	 *          bUseTimeLimit FALSE
	 *   :5708  `if (bPerformFullPurge)` -> IncrementalPurgeGarbage(false), no time limit, runs to completion
	 *   :5713  `if (bPerformFullPurge)` -> ShrinkUObjectHashTables()
	 *   :5721  `if (bPerformFullPurge)` -> FMemory::Trim()
	 * An ordinary pass does NONE of those. It sets `GObjPurgeIsRequired = true` (:5705) and leaves the
	 * unhash and the purge to `IncrementalPurgeGarbage(true, IncGCTime)` on later frames
	 * (UnrealEngine.cpp:1986-1987), budgeted 2 ms a frame by `gc.IncrementalGCTimePerFrame`
	 * (UnrealEngine.cpp:1732).
	 *
	 * So reporting ONE mean over both kinds is the wrong statistic, and the orphaned baseline shows it:
	 * "27 pauses, mean 27.2 ms, worst 148.6 ms" is a bimodal mixture wearing a single number.
	 *
	 * HOW THE READING IS TAKEN, chain verified from engine bytes rather than assumed:
	 *   UObjectGlobals.h:944   `COREUOBJECT_API bool IsIncrementalPurgePending();` -- public, no `#if`
	 *   GarbageCollection.cpp:5051-5053   body: `GObjIncrementalPurgeIsInProgress || GObjPurgeIsRequired`
	 *   GarbageCollection.cpp:5705        EVERY pass sets `GObjPurgeIsRequired = true`
	 *   GarbageCollection.cpp:5710        a full purge then completes the purge, clearing it at :5016
	 *   GarbageCollection.cpp:5733        the post broadcast fires after all of the above
	 * Therefore, read from inside OnPostGC, `IsIncrementalPurgePending() == false` IS `bPerformFullPurge`.
	 * The engine's own `bGCPerformingFullPurge` is private (Engine.h:1904, under the `private:` at :1892)
	 * and UEngine is engine rather than FactoryGame, so no AccessTransformer route exists -- this public
	 * function is the whole reason the split is knowable at all.
	 */
	int32 GFPMGCFullPurges = 0;
	double GFPMGCFullPurgeMs = 0.0;
	double GFPMGCWorstFullPurgeMs = 0.0;
	int32 GFPMGCOrdinary = 0;
	double GFPMGCOrdinaryMs = 0.0;
	double GFPMGCWorstOrdinaryMs = 0.0;

	/**
	 * ⚠ THE CROSS-CHECK THAT KEEPS BOTH HONEST. The timing classifier and the full-purge reading are
	 * INDEPENDENT, so counting where they disagree costs one int and is the only thing here that can
	 * catch either of them going wrong. It is reported unconditionally, including when it is zero,
	 * because a silent agreement is exactly what a stuck reading would look like.
	 */
	int32 GFPMGCSplitDisagreements = 0;

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

	/** Section 7.3, the other half of the watermark. See FPMGCMeter.h's FWatermark/GetWatermark
	 *  comment for the credit, the disclosure, and why this is a direct read rather than a probe. */
	int32 ObjectCapacity()
	{
		return GUObjectArray.GetObjectArrayCapacity();
	}

	/** Latched so the warning and the sticky overlay row do not need two callers to agree on the
	 *  same arithmetic - one place computes the percentage, everyone else reads the result. */
	FFPMGCMeter::FWatermark ComputeWatermark()
	{
		FFPMGCMeter::FWatermark Out;
		Out.Claimed = LiveObjects();
		Out.Capacity = ObjectCapacity();
		Out.Percent = Out.Capacity > 0 ? (100.f * static_cast<float>(Out.Claimed) / static_cast<float>(Out.Capacity)) : 0.f;
		return Out;
	}

	float TargetInterval()
	{
		return GEngine ? GEngine->GetTimeBetweenGarbageCollectionPasses() : 0.f;
	}

	/**
	 * The per-frame incremental purge budget, in seconds -- read live rather than hardcoded, because the
	 * point of quoting it is to say what THIS build is doing.
	 *
	 * ⚠ IT IS READ FROM THE CVAR, NOT FROM `UEngine::GetIncrementalGCTimePerFrame()`. That engine
	 * accessor applies the low-memory override (UnrealEngine.cpp:1869-1884) and is the honest number for
	 * "what will the next frame spend", but it is only reachable through GEngine and only meaningful
	 * once GEngine exists. The cvar is the configured value and cannot read as a misleading 0.0 from a
	 * report typed before the world is up. Returns a negative on a failed find so the caller can tell
	 * "not found" from "zero", which are very different statements.
	 * Registered unconditionally at UnrealEngine.cpp:1732-1738 with no `#if`, default 0.002 -- so unlike
	 * `gc.IncrementalGatherTimeLimit` (COMPILED OUT of Shipping at GarbageCollection.cpp:311-321) this
	 * one really is present in the retail client and the dedicated server.
	 */
	float IncrementalPurgeBudgetSeconds()
	{
		const IConsoleVariable* Budget =
			IConsoleManager::Get().FindConsoleVariable(TEXT("gc.IncrementalGCTimePerFrame"), false);
		return Budget ? Budget->GetFloat() : -1.f;
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

		// See GFPMGCFullPurges above for why this one line is a reading and the one above is a guess.
		const bool bFullPurge = !IsIncrementalPurgePending();
		if (bFullPurge)
		{
			++GFPMGCFullPurges;
			GFPMGCFullPurgeMs += PauseMs;
			GFPMGCWorstFullPurgeMs = FMath::Max(GFPMGCWorstFullPurgeMs, PauseMs);
		}
		else
		{
			++GFPMGCOrdinary;
			GFPMGCOrdinaryMs += PauseMs;
			GFPMGCWorstOrdinaryMs = FMath::Max(GFPMGCWorstOrdinaryMs, PauseMs);
		}

		// Only meaningful once the timing classifier has a previous pass to work from.
		if (SinceLast >= 0.0 && bForced != bFullPurge) { ++GFPMGCSplitDisagreements; }

		GFPMGCLastPassTime = Now;
		GFPMGCPreTime = 0.0;

		if (FPMDiag::IsOn(FPMDiag::EChannel::GCMeter))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] GC #%d: pause %.1f ms | %s | %.1f s since last (target %.1f, %s) | claimed "
				     "objects %d -> %d (%+d) | asyncloading=%d"),
				GFPMGCPasses, PauseMs,
				bFullPurge ? TEXT("FULL PURGE (read)") : TEXT("ordinary (read)"),
				SinceLast, Target,
				SinceLast < 0.0 ? TEXT("FIRST") : (bForced ? TEXT("forced?") : TEXT("timer?")),
				GFPMGCObjectsBefore, ObjectsAfter, ObjectsAfter - GFPMGCObjectsBefore, bAsync ? 1 : 0);
		}

		// A pause this long is a visible stutter, so it is worth the overlay even at low verbosity.
		if (PauseMs >= 50.0)
		{
			FPMOverlay::Post(TEXT("gc"), FString::Printf(TEXT("%.0f ms pause (#%d)"), PauseMs, GFPMGCPasses));
		}

		/*
		 * ★ SECTION 7.3, THE UOBJECT WATERMARK, SAMPLED ON THE SAME CADENCE AS EVERY OTHER READING
		 * HERE. A GC pass is a natural, already-paid-for moment to also read the crash ceiling,
		 * rather than adding a second ticker for one more read. m6164470 (every FPM feature reports
		 * to the dev overlay): a persistent gauge row every pass, rewritten in place.
		 */
		const FFPMGCMeter::FWatermark Watermark = ComputeWatermark();
		FPMOverlay::PostSticky(TEXT("gc"), TEXT("uobject-watermark"),
			FString::Printf(TEXT("UObject watermark: %d/%d (%.1f%%)"),
				Watermark.Claimed, Watermark.Capacity, Watermark.Percent));

		// GATED, unlike the overlay row above - FPMDiag.h's own rule: the ONE stated exception to
		// "0 = silent" is the armed line, and a plain UE_LOG from this file's own code can always
		// name its channel, so it does not get a second, unstated one. "Not throttled" means no
		// modulo-N suppression WITHIN whatever the channel's level allows - see FPMGCMeter.h's
		// WatermarkWarningPercent comment for why an escalating crash-ceiling warning is worth
		// repeating every pass rather than being count-suppressed.
		if (FPMDiag::IsOn(FPMDiag::EChannel::GCMeter))
		{
			UE_CLOG(Watermark.Percent >= FFPMGCMeter::WatermarkWarningPercent, LogFicsitsPerformanceManager,
				Warning,
				TEXT("[FPM]   UOBJECT WATERMARK AT %.1f%% OF CAPACITY (%d/%d, capacity is a HARD ceiling, "
				     "the process crashes when it fills, it does not degrade). 151 mods share this one "
				     "array; FPM.GC.Report shows the reading, this warning does not name a cause."),
				Watermark.Percent, Watermark.Claimed, Watermark.Capacity);
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
 * Neither sits behind `#if WITH_EDITOR`.
 *
 * ⚠ BOTH SIDES CARRY THE SAME GUARD -- corrected 2026-08-15 by re-reading the engine, because the
 * previous version of this comment claimed "the pre-side broadcast is unconditional" and that was
 * simply false:
 *     GarbageCollection.cpp:5516   if (!GIsIncrementalReachabilityPending)   <- opens, pre is INSIDE it
 *     GarbageCollection.cpp:5729   if (!GIsIncrementalReachabilityPending)   <- guards the post
 * The symmetry is good news and it is why the old note's worry was the wrong worry: a resumed
 * incremental slice skips BOTH broadcasts, so it cannot leave an unpaired pre. What it would do
 * instead is make the meter silently miss whole passes. Incremental reachability is BANNED on this
 * build (gc.AllowIncrementalReachability 1 crashed Ant's save in 34 s, and FPM2 ships no switch for
 * it), so the guard is satisfied today. Recorded as a dependency, not a certainty.
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

FFPMGCMeter::FWatermark FFPMGCMeter::GetWatermark()
{
	return ComputeWatermark();
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
	// Section 7.3: printed even when GFPMGCPasses == 0, because the watermark is a LIVE read
	// (capacity vs claimed count right now), not a per-pass accumulator; it does not need a GC
	// pass to have happened to mean something, unlike everything below this line.
	{
		const FWatermark Watermark = ComputeWatermark();
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] gc-meter: UObject watermark %d/%d (%.1f%% of capacity)%s."),
			Watermark.Claimed, Watermark.Capacity, Watermark.Percent,
			Watermark.Percent >= WatermarkWarningPercent ? TEXT(" - AT OR ABOVE the 85% warning line")
			                                              : TEXT(""));
	}

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

	/*
	 * ★★ THE TWO POPULATIONS, REPORTED SEPARATELY -- the line the single mean above was hiding.
	 *
	 * Both counts print even when one of them is zero, and that is deliberate. A stuck reading looks
	 * exactly like "every pass was the same kind", so the only way to see it is to print the bucket
	 * that did not fill. 100% in either column is a finding to be suspicious of, not a result.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   BY KIND (read, not inferred): %d FULL PURGE - mean %.1f ms, worst %.1f ms | "
		     "%d ordinary - mean %.1f ms, worst %.1f ms."),
		GFPMGCFullPurges,
		GFPMGCFullPurges > 0 ? GFPMGCFullPurgeMs / GFPMGCFullPurges : 0.0, GFPMGCWorstFullPurgeMs,
		GFPMGCOrdinary,
		GFPMGCOrdinary > 0 ? GFPMGCOrdinaryMs / GFPMGCOrdinary : 0.0, GFPMGCWorstOrdinaryMs);

	const float PurgeBudget = IncrementalPurgeBudgetSeconds();
	const FString PurgeBudgetText = PurgeBudget < 0.f
		? FString(TEXT("<cvar not found - so this build's budget is UNKNOWN, not zero>"))
		: FString::Printf(TEXT("%.1f ms a frame"), 1000.0f * PurgeBudget);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   A full purge additionally does unhash with NO time limit, the whole purge with NO "
		     "time limit, ShrinkUObjectHashTables and FMemory::Trim, all inside the pause "
		     "(GarbageCollection.cpp:5698, :5708, :5713, :5721). An ordinary pass defers the unhash and "
		     "purge to %s (gc.IncrementalGCTimePerFrame), which this meter does NOT see - so the ordinary "
		     "column UNDERSTATES the true cost of an ordinary pass."),
		*PurgeBudgetText);

	/*
	 * ⚠ THE CROSS-CHECK, AND AN HONEST STATEMENT OF WHAT IT DOES NOT MEAN.
	 *
	 * The two splits are not asking the same question, so disagreement is NOT automatically a fault.
	 * `UEngine::ForceGarbageCollection(false)` (UnrealEngine.cpp:1837-1840) forces a pass WITHOUT
	 * setting `bFullPurgeTriggered` - it just backdates the timer - so a legitimately forced pass can
	 * be an ordinary purge, and it SHOULD show up here as a disagreement. That is a real observation
	 * about what forced the pass, not an instrument fault.
	 * What the count is genuinely good for is the other direction: it is the only line that can show
	 * the two estimates moving independently at all. Printed either way, because "no disagreements" is
	 * also what a stuck reading produces and the reader has to be able to tell.
	 */
	UE_CLOG(GFPMGCSplitDisagreements > 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d of %d pass(es): the timing guess and the full-purge reading differ. Expected, "
		     "NOT a fault - ForceGarbageCollection(false) forces a pass that is still an ordinary purge "
		     "(UnrealEngine.cpp:1837-1840). Read it as 'these were forced but cheap'. Where the two "
		     "MUST agree is nowhere; only the full-purge column is a reading."),
		GFPMGCSplitDisagreements, GFPMGCPasses);

	UE_CLOG(GFPMGCSplitDisagreements == 0 && GFPMGCPasses > 1, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   the timing guess and the full-purge reading never differed across %d pass(es). "
		     "With more than a few passes that is suspicious rather than reassuring - two independent "
		     "estimates agreeing perfectly is also what one stuck reading looks like."),
		GFPMGCPasses);

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

static FAutoConsoleCommandWithOutputDevice GFPMGCReportCmd(
	TEXT("FPM.GC.Report"),
	TEXT("Garbage collection: pass count, mean and worst pause, and the timer-vs-forced split that "
	     "decides whether the pacing lever is worth writing."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMGCMeter::ReportNow();
	}));
