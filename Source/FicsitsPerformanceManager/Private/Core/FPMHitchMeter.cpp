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

	FlushHandle   = FCoreDelegates::OnAsyncLoadingFlush.AddRaw(this, &FFPMHitchMeter::OnAsyncLoadingFlush);
	PackageHandle = FCoreDelegates::GetOnAsyncLoadPackage().AddRaw(this, &FFPMHitchMeter::OnAsyncLoadPackage);

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
}

void FFPMHitchMeter::OnWorldLoad(UWorld* World)
{
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

	++FramesInWindow;
	++FramesTotal;
	WindowSeconds += FrameMs / 1000.0;

	// Set() returns the old value, so the read and the reset are one operation and a flush landing between
	// them cannot be lost.
	const int32 FlushesThisFrame = FlushesInFrame.Set(0);

	const float ThresholdMs = CVarHitchThresholdMs.GetValueOnAnyThread();
	const float CeilingMs   = CVarHitchIgnoreAboveMs.GetValueOnAnyThread();

	if (FrameMs >= CeilingMs)
	{
		++LoadStalls;
	}
	else if (FrameMs >= ThresholdMs)
	{
		++Hitches;
		++SessionHitches;
		HitchMsTotal += FrameMs;
		WorstHitchMs   = FMath::Max(WorstHitchMs, FrameMs);
		SessionWorstMs = FMath::Max(SessionWorstMs, FrameMs);
		if (FlushesThisFrame > 0) { ++HitchesWithFlush; }

		if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch))
		{
			if (LinesThisWindow < MaxLinesPerWindow)
			{
				++LinesThisWindow;

				FString Packages;
				if (FPMDiag::IsOn(FPMDiag::EChannel::Hitch, 2))
				{
					FScopeLock Lock(&PackagesLock);
					Packages = RecentPackages.Num() > 0
						? FString::Printf(TEXT(" | in flight: %s"), *FString::Join(RecentPackages, TEXT(", ")))
						: FString(TEXT(" | in flight: none recorded"));
				}

				// Warning, not Display: a hitch is the thing being hunted, and Warning is what survives a
				// default log filter when Ant sends the file over.
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] HITCH %.1f ms - %d async-load flush(es) in this frame, %d package(s) still "
					     "loading%s"),
					FrameMs, FlushesThisFrame, GetNumAsyncPackages(), *Packages);
			}
			else
			{
				++LinesSuppressed;
			}
		}
	}

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
		Line += FString::Printf(TEXT(" | worst %.1f ms, mean %.1f ms | %d of them had an async-load flush"),
			WorstHitchMs, MeanMs, HitchesWithFlush);
	}
	if (LoadStalls > 0)
	{
		Line += FString::Printf(TEXT(" | %d load stall(s) over %.0f ms, not counted as hitches"),
			LoadStalls, CVarHitchIgnoreAboveMs.GetValueOnAnyThread());
	}
	Line += FString::Printf(TEXT(" | %d flush(es) total this session"), FlushesTotal.GetValue());
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
