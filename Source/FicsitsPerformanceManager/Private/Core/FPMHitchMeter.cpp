// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMHitchMeter.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "ShaderPipelineCache.h"
#include "UObject/UObjectGlobals.h"

/*
 * THE THRESHOLD IS A CVAR AND ITS HELP TEXT CARRIES THE TRAP, because the trap is not obvious and it turns
 * the instrument into a liar rather than merely mis-tuning it.
 */
static TAutoConsoleVariable<float> CVarHitchThresholdMs(
	TEXT("FPM.Hitch.ThresholdMs"), 50.0f,
	TEXT("A frame at or above this many milliseconds counts as a hitch. ⚠ It MUST sit above your frame-cap "
	     "period or every capped frame reads as a hitch: 60 fps is 16.7 ms, 30 fps is 33.3 ms. The measured "
	     "value is wall clock between engine ticks, so it includes the vsync/cap wait by construction. "
	     "Default 50."),
	ECVF_Default);

/*
 * A LEVEL LOAD IS NOT A HITCH, AND CALLING IT ONE WOULD SWAMP THE THING WE ARE HUNTING. But it is not
 * dropped either — silently discarding samples is how an instrument starts lying, and this project already
 * has four instrument-gap incidents on the board. Stalls get their own counter and their own column.
 */
static TAutoConsoleVariable<float> CVarHitchIgnoreAboveMs(
	TEXT("FPM.Hitch.IgnoreAboveMs"), 1000.0f,
	TEXT("A frame at or above this counts as a LOAD STALL rather than a hitch - a level load, an alt-tab, a "
	     "debugger break. Counted and reported separately, never discarded. Default 1000."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarHitchSummarySeconds(
	TEXT("FPM.Hitch.SummarySeconds"), 60.0f,
	TEXT("Seconds between running summaries. Every summary carries the frame count it was measured over, so "
	     "a dead meter reads as 0-in-0 rather than as a calm session. Default 60."),
	ECVF_Default);

FFPMHitchMeter& FFPMHitchMeter::Get()
{
	static FFPMHitchMeter Instance;
	return Instance;
}

void FFPMHitchMeter::Arm()
{
	// Idempotent. Arm() is called once from StartupModule today, but a meter that double-subscribes would
	// double-count flushes and there would be nothing in the output to reveal it.
	if (TickHandle.IsValid()) { return; }

	bPrimed = false;
	FlushesInFrame.Reset();

	// 0.f delay = every engine tick. `FEngineLoop::Tick` drives the core ticker once per frame
	// (`LaunchEngineLoop.cpp:5852`), which is what makes "one sample per frame" true rather than hopeful.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFPMHitchMeter::Tick), 0.f);

	FlushHandle    = FCoreDelegates::OnAsyncLoadingFlush.AddRaw(this, &FFPMHitchMeter::OnAsyncLoadingFlush);
	PackageHandle  = FCoreDelegates::GetOnAsyncLoadPackage().AddRaw(this, &FFPMHitchMeter::OnAsyncLoadPackage);
	SyncLoadHandle = FCoreDelegates::OnSyncLoadPackage.AddRaw(this, &FFPMHitchMeter::OnSyncLoadPackage);
	PreGcHandle    = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddRaw(this, &FFPMHitchMeter::OnPreGarbageCollect);

	// ★ LAMBDAS RATHER THAN AddRaw, and for one specific reason: the context parameter's type
	// `FShaderPipelineCache::FShaderCachePrecompileContext` is a NESTED class (`ShaderPipelineCache.h:158`),
	// so it cannot be forward-declared, and a member-function binding would drag a RenderCore header into
	// FPM's public header for a parameter neither callback wants. The lambda drops it here instead. The
	// lifetime contract is identical to the four AddRaw calls above — both handles are removed in Disarm().
	PsoBeginHandle = FShaderPipelineCache::GetPrecompilationBeginDelegate().AddLambda(
		[this](uint32 Count, const FShaderPipelineCache::FShaderCachePrecompileContext&)
		{ OnPsoPrecompileBegin(Count); });
	PsoCompleteHandle = FShaderPipelineCache::GetPrecompilationCompleteDelegate().AddLambda(
		[this](uint32 Count, double Seconds, const FShaderPipelineCache::FShaderCachePrecompileContext&)
		{ OnPsoPrecompileComplete(Count, Seconds); });

	// Not gated by the channel: this is the stated Arm()-line exception in FPMDiag.h, and it is the line
	// that distinguishes "measured nothing" from "never measured".
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] hitch meter armed - threshold %.1f ms, stall ceiling %.1f ms, summary every %.0f s. "
		     "The engine's own detector is compiled out of this build (ALLOW_HITCH_DETECTION=0), so this is "
		     "the only frame-time instrument present."),
		CVarHitchThresholdMs.GetValueOnAnyThread(),
		CVarHitchIgnoreAboveMs.GetValueOnAnyThread(),
		CVarHitchSummarySeconds.GetValueOnAnyThread());
}

void FFPMHitchMeter::Disarm()
{
	LogSummary(TEXT("shutdown"));

	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (FlushHandle.IsValid())
	{
		FCoreDelegates::OnAsyncLoadingFlush.Remove(FlushHandle);
		FlushHandle.Reset();
	}
	if (PackageHandle.IsValid())
	{
		FCoreDelegates::GetOnAsyncLoadPackage().Remove(PackageHandle);
		PackageHandle.Reset();
	}
	if (SyncLoadHandle.IsValid())
	{
		FCoreDelegates::OnSyncLoadPackage.Remove(SyncLoadHandle);
		SyncLoadHandle.Reset();
	}
	if (PreGcHandle.IsValid())
	{
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGcHandle);
		PreGcHandle.Reset();
	}
	if (PsoBeginHandle.IsValid())
	{
		FShaderPipelineCache::GetPrecompilationBeginDelegate().Remove(PsoBeginHandle);
		PsoBeginHandle.Reset();
	}
	if (PsoCompleteHandle.IsValid())
	{
		FShaderPipelineCache::GetPrecompilationCompleteDelegate().Remove(PsoCompleteHandle);
		PsoCompleteHandle.Reset();
	}
}

void FFPMHitchMeter::OnPsoPrecompileBegin(uint32 Count)
{
	bPsoRunActive.store(true, std::memory_order_relaxed);

	// ⚠ RENDER THREAD (`FShaderPipelineCache : FTickableObjectRenderThread`, `ShaderPipelineCache.h:78`).
	// UE_LOG is thread-safe and `FPMDiag::IsOn` is already called off the game thread by
	// `OnAsyncLoadPackage` in this same file. `FPMOverlay` is NOT, so nothing here posts to the screen —
	// the summary does that, from the game thread, where it belongs.
	if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] PSO precompile run STARTED - %u task(s) queued. Spans from here until it finishes "
			     "are counted against their own denominator, so the in-run hitch rate is comparable to the "
			     "out-of-run one rather than just larger."), Count);
	}
}

void FFPMHitchMeter::OnPsoPrecompileComplete(uint32 Count, double Seconds)
{
	bPsoRunActive.store(false, std::memory_order_relaxed);
	PsoRunsCompleted.fetch_add(1, std::memory_order_relaxed);
	PsoTasksLastRun.store(Count, std::memory_order_relaxed);
	PsoSecondsLastRun.store(Seconds, std::memory_order_relaxed);

	// The wording is load-bearing. `Seconds` is `TotalPrecompileTime`, summed per completed task across the
	// whole run (`ShaderPipelineCache.cpp:1204`) and reset right after this broadcast (`:1826-1827`). Calling
	// it a stall would be the exact species of overstatement this meter was built to stop.
	if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] PSO precompile run FINISHED - %u task(s), %.2f s of compile time summed across the "
			     "whole run. That is NOT one frame's cost and is not attributable to any single frame."),
			Count, Seconds);
	}
}

void FFPMHitchMeter::OnWorldLoad(UWorld* World)
{
	/*
	 * ⚠ CLOSE THE OPEN SPAN BEFORE RE-PRIMING — review finding A, 2026-08-09.
	 *
	 * The first version just set `bPrimed = false` here. But this is dispatched from the game world
	 * module's CONSTRUCTION phase (`RootGameWorld_FicsitsPerformanceManager.cpp:61-64`) on the SAME game
	 * thread, and it can land inside a span that no `Tick()` has closed yet — a slow UI or gameplay
	 * callstack that itself triggers the load. Re-priming without measuring that span made `Tick()` take
	 * its `!bPrimed` branch and report NOTHING for it: neither hitch nor stall. So the meter could MISS a
	 * real hitch precisely when one coincided with a load boundary, which is a plausible place for one.
	 *
	 * An instrument that silently drops the sample it was least likely to see is the failure this whole
	 * file exists to argue against, so the span is classified first and re-primed second.
	 */
	if (bPrimed)
	{
		const double Now = FPlatformTime::Seconds();
		ClassifySpan((Now - LastTickSeconds) * 1000.0, FlushesInFrame.Set(0), /*bClosedByLoad*/ true,
			SyncLoadsInFrame.Set(0), GcInFrame.Set(0));
	}

	// Close the books on the previous world before the clock jumps, so its numbers are attributable to it.
	if (FramesTotal > 0)
	{
		LogSummary(TEXT("world load"));
	}
	bPrimed = false;
}

void FFPMHitchMeter::OnAsyncLoadingFlush()
{
	FlushesInFrame.Increment();
	FlushesTotal.Increment();
}

void FFPMHitchMeter::OnPreGarbageCollect()
{
	GcInFrame.Increment();
	GcTotal.Increment();
}

void FFPMHitchMeter::OnSyncLoadPackage(const FString& PackageName)
{
	SyncLoadsInFrame.Increment();
	SyncLoadsTotal.Increment();

	// Kept unconditionally, not behind the verbose gate: this is ONE FString assignment per synchronous
	// package load, and a sync load is by definition already the expensive thing in that frame. Paying a
	// copy to be able to NAME it is the trade this instrument exists to make.
	FScopeLock Lock(&SyncNameLock);
	LastSyncPackage = PackageName;

	/*
	 * ★ COUNT PER PACKAGE, BECAUSE "last=" IS A BIASED SAMPLE — added 2026-08-09 the moment I tried to
	 * act on it.
	 *
	 * The hitch line reports the LAST sync load of its span. One measured span contained 283 of them and
	 * named exactly one. So the seven names harvested from a session are "whatever finished last", not
	 * "what costs the most" — and choosing what to pin from that list would be choosing by an artefact of
	 * the reporting, which is the same class of mistake as reading a count off a grep that matched my own
	 * log line. A frequency map costs one hash lookup per sync load and answers the question actually
	 * being asked: WHICH packages block, and how often.
	 */
	SyncLoadCounts.FindOrAdd(PackageName)++;
}

void FFPMHitchMeter::OnAsyncLoadPackage(FStringView PackageName)
{
	// ⚠ THE GATE IS THE POINT. This fires for EVERY async load in the session, on whichever thread issued
	// it. At level 1 the cost is one int compare and a return; only at verbose do we pay for a string copy
	// and a lock. FPMDiag.h states this shape as the required one, after 0.58.54 froze the game by building
	// log strings before checking whether anyone wanted them.
	if (!FPMDiag::IsOn(FPMDiag::EChannel::Hitch, 2)) { return; }

	FScopeLock Lock(&PackagesLock);
	if (RecentPackages.Num() >= MaxRecentPackages)
	{
		RecentPackages.RemoveAt(0, 1, EAllowShrinking::No);
	}
	RecentPackages.Emplace(PackageName);
}

void FFPMHitchMeter::ClassifySpan(double SpanMs, int32 FlushesInSpan, bool bClosedByLoad,
                                  int32 SyncLoadsInSpan, int32 GcInSpan)
{
	// A world-load-closed span is counted as one sample like any other. It is genuine elapsed game-thread
	// time; the only difference is which event closed it, and there is at most one per world load.
	++FramesInWindow;
	++FramesTotal;
	WindowSeconds += SpanMs / 1000.0;

	/*
	 * ★ READ, NOT CONSUMED — and counted for EVERY span, not only the hitching ones.
	 *
	 * The other three inputs arrive as parameters because they are read-and-reset atomics that must be
	 * consumed exactly once per span. This one is a LEVEL that spans many frames, so it is read here
	 * instead, which also leaves both `ClassifySpan` call sites untouched.
	 *
	 * `FramesDuringPso` is the whole reason the bucket is worth having. A precompile run can cover an entire
	 * window; "these 18 hitches happened during it" would then be true, useless, and easy to mistake for a
	 * finding. With the denominator it becomes a rate that can be compared against the out-of-run rate, and
	 * a rate that is NOT elevated is a real answer too.
	 */
	const bool bPsoInSpan = bPsoRunActive.load(std::memory_order_relaxed);
	if (bPsoInSpan) { ++FramesDuringPso; }

	// The point of the widening: a span that matched nothing is now counted rather than merely absent from
	// four other counters. Design `:1218` -- "most stalls were anonymous BY CONSTRUCTION".
	const bool bAttributed = FlushesInSpan > 0 || SyncLoadsInSpan > 0 || GcInSpan > 0 || bPsoInSpan;

	const float ThresholdMs = CVarHitchThresholdMs.GetValueOnAnyThread();
	const float CeilingMs   = CVarHitchIgnoreAboveMs.GetValueOnAnyThread();

	if (SpanMs >= CeilingMs)
	{
		++LoadStalls;

		/*
		 * ⚠ STALLS CARRY THEIR FLUSH COUNT TOO — review finding B, 2026-08-09, and it was a HIGH.
		 *
		 * The first version incremented `LoadStalls` and threw `FlushesInSpan` away, so any span over the
		 * ceiling lost its flush attribution permanently — while the count had already been computed one
		 * line earlier and was sitting right there. That is precisely backwards: the header's whole claim
		 * is ATTRIBUTION, NOT ADJACENCY, and the severe tail is where a synchronous load is MOST likely to
		 * be the mechanism. The statistic would have silently stopped covering exactly the cases the
		 * hypothesis lives or dies on.
		 */
		if (FlushesInSpan > 0)   { ++LoadStallsWithFlush; }
		if (SyncLoadsInSpan > 0) { ++StallsWithSyncLoad; }
		if (GcInSpan > 0)        { ++StallsWithGc; }
		if (bPsoInSpan)          { ++StallsWithPso; }
		if (!bAttributed)        { ++StallsUnattributed; }
		return;
	}

	if (SpanMs < ThresholdMs) { return; }

	++Hitches;
	++SessionHitches;
	HitchMsTotal += SpanMs;
	WorstHitchMs   = FMath::Max(WorstHitchMs, SpanMs);
	SessionWorstMs = FMath::Max(SessionWorstMs, SpanMs);
	if (FlushesInSpan > 0)   { ++HitchesWithFlush; }
	if (SyncLoadsInSpan > 0) { ++HitchesWithSyncLoad; }
	if (GcInSpan > 0)        { ++HitchesWithGc; }
	if (bPsoInSpan)          { ++HitchesWithPso; }
	if (!bAttributed)        { ++HitchesUnattributed; }

	if (!FPMDiag::IsOn(FPMDiag::EChannel::Hitch)) { return; }

	if (LinesThisWindow >= MaxLinesPerWindow)
	{
		++LinesSuppressed;
		return;
	}
	++LinesThisWindow;

	FString Packages;
	if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch, 2))
	{
		FScopeLock Lock(&PackagesLock);
		Packages = RecentPackages.Num() > 0
			? FString::Printf(TEXT(" | in flight: %s"), *FString::Join(RecentPackages, TEXT(", ")))
			: FString(TEXT(" | in flight: none recorded"));
	}

	// Warning, not Display: a hitch is the thing being hunted, and Warning is what survives a default log
	// filter when Ant sends the file over.
	// The sync-load half is named, because it is the half that can be. When it fires, that package name IS
	// the answer to "what blocked this frame" -- no correlation step, no adjacency argument.
	FString SyncPart;
	if (SyncLoadsInSpan > 0)
	{
		FScopeLock Lock(&SyncNameLock);
		SyncPart = FString::Printf(TEXT(" | %d SYNC LOAD(S), last='%s'"), SyncLoadsInSpan, *LastSyncPackage);
	}

	// ⚠ THE TWO NEW FIELDS ARE WORDED DOWN, DELIBERATELY. The sync-load half names a package and is a direct
	// answer; the PSO half is an overlap with a run that may have covered the whole window, so it says
	// "during", not "because of". And a span that matched nothing is stamped UNATTRIBUTED so the anonymous
	// ones are greppable in the log rather than only countable in the summary.
	const TCHAR* PsoPart = bPsoInSpan ? TEXT(" | during a PSO precompile run") : TEXT("");
	const TCHAR* UnattributedPart = bAttributed ? TEXT("") : TEXT(" | UNATTRIBUTED");

	UE_LOG(LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] HITCH %.1f ms%s - %d async-load flush(es), %d GC pass(es) in this span, %d package(s) "
		     "still loading%s%s%s%s"),
		SpanMs, bClosedByLoad ? TEXT(" (closed by a world load)") : TEXT(""),
		FlushesInSpan, GcInSpan, GetNumAsyncPackages(), *SyncPart, PsoPart, UnattributedPart, *Packages);
}

bool FFPMHitchMeter::Tick(float /* SmoothedEngineDeltaDoNotUse */)
{
	const double Now = FPlatformTime::Seconds();

	// First sample after arm or after a world load has no meaningful predecessor. Consume the flushes that
	// accumulated during it too — attributing a loading screen's flushes to the first playable frame would
	// manufacture exactly the correlation this meter exists to test honestly.
	if (!bPrimed)
	{
		LastTickSeconds = Now;
		bPrimed = true;
		FlushesInFrame.Set(0);
		return true;
	}

	const double FrameMs = (Now - LastTickSeconds) * 1000.0;
	LastTickSeconds = Now;

	// Set() returns the old value (`ThreadSafeCounter.h:99-102`), so the read and the reset are one atomic
	// operation and a flush landing between them cannot be lost.
	ClassifySpan(FrameMs, FlushesInFrame.Set(0), /*bClosedByLoad*/ false, SyncLoadsInFrame.Set(0),
		GcInFrame.Set(0));

	if (WindowSeconds >= CVarHitchSummarySeconds.GetValueOnAnyThread())
	{
		LogSummary(TEXT("running"));
	}

	return true;
}

void FFPMHitchMeter::LogSyncPackages()
{
	// Copied out under the lock, then sorted outside it: the sort is O(n log n) with a string compare and
	// has no business holding a lock that a cross-thread load callback is waiting on.
	TArray<TPair<FString, int32>> Ranked;
	{
		FScopeLock Lock(&SyncNameLock);
		Ranked.Reserve(SyncLoadCounts.Num());
		for (const TPair<FString, int32>& Pair : SyncLoadCounts) { Ranked.Add(Pair); }
	}
	Ranked.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
		{ return A.Value > B.Value; });

	int32 Total = 0;
	for (const TPair<FString, int32>& Pair : Ranked) { Total += Pair.Value; }

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] sync loads: %d distinct package(s), %d load(s) total this session. "
		     "Each one BLOCKED the game thread. Top 25:"),
		Ranked.Num(), Total);

	const int32 Show = FMath::Min(Ranked.Num(), 25);
	for (int32 i = 0; i < Show; ++i)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %4d x  %s"), Ranked[i].Value, *Ranked[i].Key);
	}
	// Stated, never silent -- a truncated list that does not say it truncated reads as the whole list.
	UE_CLOG(Ranked.Num() > Show, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ... and %d more package(s) not shown."), Ranked.Num() - Show);
}

void FFPMHitchMeter::LogSummary(const TCHAR* Reason)
{
	/*
	 * ★ THE DENOMINATOR IS NOT DECORATION. `0 hitches` and `0 hitches in 4,412 frames over 61.2 s` are
	 * different claims, and only the second one is evidence. The whole reason this instrument exists is that
	 * three earlier measurements on this project reported a zero that no emitter could ever have made
	 * non-zero. This line is built so that failure is visible in the line itself.
	 */
	const double MeanMs = Hitches > 0 ? (HitchMsTotal / Hitches) : 0.0;

	FString Line = FString::Printf(
		TEXT("%s | %d hitch(es) in %llu frame(s) over %.1f s (threshold %.0f ms)"),
		Reason, Hitches, static_cast<unsigned long long>(FramesInWindow), WindowSeconds,
		CVarHitchThresholdMs.GetValueOnAnyThread());

	if (Hitches > 0)
	{
		// BOTH halves, always, and never folded together. The async-flush count alone was structurally
		// blind to sync loads, so reporting it on its own is what made a partial zero look like a whole one.
		Line += FString::Printf(
			TEXT(" | worst %.1f ms, mean %.1f ms | %d had an async-load flush, %d had a SYNC load, %d had a "
			     "GC, %d were during a PSO precompile"),
			WorstHitchMs, MeanMs, HitchesWithFlush, HitchesWithSyncLoad, HitchesWithGc, HitchesWithPso);

		/*
		 * ★ THE UNATTRIBUTED SHARE IS THE NUMBER THIS WIDENING LIVES OR DIES BY, so it is printed as a
		 * PERCENTAGE and not left to be worked out from the four counters above.
		 *
		 * Design `:1218` asks for exactly this: "an unattributed-stall RATE, printed, so 'most stalls
		 * anonymous' becomes a number that can fall." Without it, adding a fifth bucket reads as progress
		 * whether or not the anonymous share actually moved -- and on 0.6.0 that share was 21 of 21.
		 * `Hitches > 0` is guaranteed inside this branch, so the division is safe.
		 */
		Line += FString::Printf(TEXT(" | %d UNATTRIBUTED (%.0f%% of hitches)"),
			HitchesUnattributed, 100.0 * HitchesUnattributed / Hitches);
	}

	/*
	 * ★ THE RATE COMPARISON IS THE PSO BUCKET'S ENTIRE VALUE, and without it the bucket would be one more
	 * coincidence count. "18 hitches during a precompile run" says nothing when the run covered the window;
	 * "18 in 40 spans inside it versus 3 in 4,200 outside" is a finding. Printed only when a run actually
	 * overlapped this window, so a normal window stays quiet.
	 */
	if (FramesDuringPso > 0)
	{
		const uint64 FramesOutside = FramesInWindow > static_cast<uint64>(FramesDuringPso)
			? FramesInWindow - static_cast<uint64>(FramesDuringPso)
			: 0;
		const int32 HitchesOutside = Hitches - HitchesWithPso;
		Line += FString::Printf(
			TEXT(" | PSO precompile overlapped %d span(s): %d hitch(es) there (%.2f%%) vs %d in the other "
			     "%llu (%.2f%%)"),
			FramesDuringPso, HitchesWithPso, 100.0 * HitchesWithPso / FramesDuringPso,
			HitchesOutside, static_cast<unsigned long long>(FramesOutside),
			FramesOutside > 0 ? 100.0 * HitchesOutside / static_cast<double>(FramesOutside) : 0.0);
	}
	if (LoadStalls > 0)
	{
		// The stalls carry their own flush count. Folding them into the hitch figure would overstate the
		// hitch rate; dropping the count entirely is what review finding B caught.
		Line += FString::Printf(
			TEXT(" | %d load stall(s) over %.0f ms (%d flush, %d sync, %d gc, %d pso, %d UNATTRIBUTED), not "
			     "counted as hitches"),
			LoadStalls, CVarHitchIgnoreAboveMs.GetValueOnAnyThread(), LoadStallsWithFlush, StallsWithSyncLoad,
			StallsWithGc, StallsWithPso, StallsUnattributed);
	}
	Line += FString::Printf(TEXT(" | session totals: %d flush(es), %d sync load(s), %d GC pass(es)"),
		FlushesTotal.GetValue(), SyncLoadsTotal.GetValue(), GcTotal.GetValue());

	// Run-level context, appended only once a run has actually completed. The seconds figure is labelled as
	// compile time rather than stall time every place it is printed, because it is the former.
	if (const int32 PsoRuns = PsoRunsCompleted.load(std::memory_order_relaxed); PsoRuns > 0)
	{
		Line += FString::Printf(
			TEXT(", %d PSO precompile run(s) (last: %u task(s), %.2f s compile time)"),
			PsoRuns, PsoTasksLastRun.load(std::memory_order_relaxed),
			PsoSecondsLastRun.load(std::memory_order_relaxed));
	}
	if (LinesSuppressed > 0)
	{
		// Stated, never silent. A capped log that does not say it capped reads as a quiet session.
		Line += FString::Printf(TEXT(" | %d hitch line(s) SUPPRESSED by the per-window cap"), LinesSuppressed);
	}

	// Post writes to the screen AND the log, so this must not also UE_LOG or every summary appears twice.
	// Gated on the channel because the master switch alone is not per-channel control.
	//
	// ★ STICKY, KEYED ON Reason. Ant, 2026-08-09: "i also need a way to reset this window, since it just
	// prints forever." This summary is a GAUGE -- it always has a current reading -- and appending one
	// every 60 s filled all 18 panel rows with hitch history inside twenty minutes, scrolling the startup
	// and rain-sweep lines off the top. `Reason` is exactly the right slot key and needs no new state:
	// "running" keeps ONE row that updates in place, while "world load" is a different Reason and so keeps
	// its own row rather than being overwritten by the next rolling window.
	//
	// The LOG still receives every summary. Only the screen row is replaced -- the series is the thing you
	// want when reading a session back, and it is the half that cannot be reconstructed afterwards.
	if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch))
	{
		FPMOverlay::PostSticky(TEXT("hitch meter"), Reason, Line);
	}

	// Window resets; session totals do not.
	FramesInWindow = 0;
	WindowSeconds = 0.0;
	Hitches = 0;
	HitchesWithFlush = 0;
	LoadStalls = 0;
	LoadStallsWithFlush = 0;
	HitchesWithSyncLoad = 0;
	StallsWithSyncLoad = 0;
	HitchesWithGc = 0;
	StallsWithGc = 0;
	// ⚠ `bPsoRunActive` is NOT reset here — it is a level owned by the render thread's Begin/Complete pair,
	// and clearing it on a window boundary would fake the end of a run that is still going. Only its
	// per-window counters reset.
	FramesDuringPso = 0;
	HitchesWithPso = 0;
	StallsWithPso = 0;
	HitchesUnattributed = 0;
	StallsUnattributed = 0;
	LinesThisWindow = 0;
	LinesSuppressed = 0;
	WorstHitchMs = 0.0;
	HitchMsTotal = 0.0;
}

/*
 * `FPM.Hitch.Report` — because a running summary you have to wait sixty seconds for is not the one you want
 * the moment after a hitch happens. This also prints the SESSION totals, which the windowed line never does.
 */
/*
 * `FPM.Hitch.Packages` — the ranked list the hitch lines cannot give you.
 *
 * This is what a pin list must be chosen FROM. The per-hitch `last=` field names whichever package
 * happened to finish last in that span, so harvesting names from the log ranks by coincidence. This ranks
 * by count.
 */
static FAutoConsoleCommand GHitchPackagesCmd(
	TEXT("FPM.Hitch.Packages"),
	TEXT("Print the synchronously-loaded packages this session, most frequent first."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMHitchMeter::Get().LogSyncPackages();
	}));

static FAutoConsoleCommand GHitchReportCmd(
	TEXT("FPM.Hitch.Report"),
	TEXT("Print the FPM hitch meter's running totals now, plus the session totals."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMHitchMeter::Get().LogSummary(TEXT("on request"));
	}));
