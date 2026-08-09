// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMHitchMeter.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
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

	UE_LOG(LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] HITCH %.1f ms%s - %d async-load flush(es), %d GC pass(es) in this span, %d package(s) "
		     "still loading%s%s"),
		SpanMs, bClosedByLoad ? TEXT(" (closed by a world load)") : TEXT(""),
		FlushesInSpan, GcInSpan, GetNumAsyncPackages(), *SyncPart, *Packages);
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
			TEXT(" | worst %.1f ms, mean %.1f ms | %d had an async-load flush, %d had a SYNC load, %d had a GC"),
			WorstHitchMs, MeanMs, HitchesWithFlush, HitchesWithSyncLoad, HitchesWithGc);
	}
	if (LoadStalls > 0)
	{
		// The stalls carry their own flush count. Folding them into the hitch figure would overstate the
		// hitch rate; dropping the count entirely is what review finding B caught.
		Line += FString::Printf(
			TEXT(" | %d load stall(s) over %.0f ms (%d flush, %d sync, %d gc), not counted as hitches"),
			LoadStalls, CVarHitchIgnoreAboveMs.GetValueOnAnyThread(), LoadStallsWithFlush, StallsWithSyncLoad,
			StallsWithGc);
	}
	Line += FString::Printf(TEXT(" | session totals: %d flush(es), %d sync load(s), %d GC pass(es)"),
		FlushesTotal.GetValue(), SyncLoadsTotal.GetValue(), GcTotal.GetValue());
	if (LinesSuppressed > 0)
	{
		// Stated, never silent. A capped log that does not say it capped reads as a quiet session.
		Line += FString::Printf(TEXT(" | %d hitch line(s) SUPPRESSED by the per-window cap"), LinesSuppressed);
	}

	// Post writes to the screen AND the log, so this must not also UE_LOG or every summary appears twice.
	// Gated on the channel because the master switch alone is not per-channel control.
	if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch))
	{
		FPMOverlay::Post(TEXT("hitch meter"), Line);
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
	LinesThisWindow = 0;
	LinesSuppressed = 0;
	WorstHitchMs = 0.0;
	HitchMsTotal = 0.0;
}

/*
 * `FPM.Hitch.Report` — because a running summary you have to wait sixty seconds for is not the one you want
 * the moment after a hitch happens. This also prints the SESSION totals, which the windowed line never does.
 */
static FAutoConsoleCommand GHitchReportCmd(
	TEXT("FPM.Hitch.Report"),
	TEXT("Print the FPM hitch meter's running totals now, plus the session totals."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMHitchMeter::Get().LogSummary(TEXT("on request"));
	}));
