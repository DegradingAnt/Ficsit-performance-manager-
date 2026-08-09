// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeCounter.h"

#include "Core/FPMFixContract.h"

/**
 * HITCH METER — frame-time measurement, because nothing in this game measures it.
 *
 * Ant, 2026-08-09, in-game on 0.3.1 (primary — she is the observer): *"game hitches when opening menus and
 * such and sometimes when moving fast through the world"*, and earlier *"i got a big hitch a few min ago.
 * maybe visible in logs"*. It was not visible in logs, and that is the entire reason this file exists.
 *
 * ★ THE ENGINE'S OWN HITCH DETECTOR IS COMPILED OUT OF THE RETAIL GAME, VERIFIED FROM THE BUILD'S OWN
 * PREPROCESSOR DEFINITIONS RATHER THAN ASSUMED. `FGameThreadHitchHeartBeat` is guarded by
 * `USE_HITCH_DETECTION`, which is `(ALLOW_HITCH_DETECTION && …)` (`Misc/Build.h:454`), and
 * `ALLOW_HITCH_DETECTION` defaults to **0** (`Misc/Build.h:439-441`) and is never redefined anywhere in this
 * engine tree. The shipped client's own shared-definitions header
 * (`Intermediate/Build/Win64/x64/FactoryGameSteam/Shipping/Engine/SharedDefinitions.Engine.Project.…h`)
 * carries `UE_BUILD_SHIPPING 1` and `WITH_EDITORONLY_DATA 0` and **no `ALLOW_HITCH_DETECTION` line at all**,
 * so it takes that 0. Consequence worth stating plainly, because it is counter-intuitive and someone will
 * otherwise try it: **`-hitchdetection=50` on the command line and the `[Core.System]`
 * `GameThreadHeartBeatHitchDuration` ini key both do NOTHING in this build.** The detector is not switched
 * off, it is absent from the binary.
 *
 * ⚠ SO THIS IS THE FOURTH INSTRUMENT GAP IN TWO DAYS, and the pattern is the point. Zero-saturation came
 * from a `LogNetTraffic` line that a Warning-default category suppresses; the rain proof needed an older log
 * to establish its category emits at all; the hitch question had no instrument whatsoever. **A zero that
 * reproduces is still a zero that means nothing if the emitter never fires.** Everything below is shaped to
 * make that impossible HERE: every summary carries its DENOMINATOR, so `0 hitches in 0 frames` is legible as
 * a dead meter rather than as a calm session.
 *
 * ★ WHY WE TIME WITH `FPlatformTime::Seconds()` AND IGNORE THE DELTA THE TICKER IS HANDED. The core ticker
 * is driven by `FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime())` (`LaunchEngineLoop.cpp:5852`), and
 * `FApp::GetDeltaTime()` is the SMOOTHED, RANGE-CLAMPED delta — `UEngine::bSmoothFrameRate` /
 * `SmoothedFrameRateRange` (`Engine.h:1552`, `:1564`). Smoothing exists precisely to hide spikes from
 * gameplay code. An instrument built on it would report a tidy number across the exact event it was built to
 * catch, which is this project's recurring failure wearing a fourth costume. We take wall clock ourselves.
 *
 * ★ ATTRIBUTION, NOT ADJACENCY. `m6249889` measured 54 blocking `FlushAsyncLoading` calls in ~14 minutes and
 * found two log signatures that always PRECEDE them — and said so with the caveat that adjacency is not
 * causation. It cannot be settled by reading more log lines. So the meter subscribes to
 * `FCoreDelegates::OnAsyncLoadingFlush` (`CoreDelegates.h:105`, broadcast from inside the game-thread flush
 * at `AsyncLoading2.cpp:11175`) and reports, per hitching frame, whether a flush happened INSIDE that frame.
 * That converts "these lines appear near each other" into a counted coincidence rate: if hitches-with-flush
 * tracks hitches, the flush is the mechanism; if it does not, the whole `FlushAsyncLoading` lead is dead and
 * we stop spending on it.
 *
 * At verbose it also names the packages, via the thread-safe `FCoreDelegates::GetOnAsyncLoadPackage()`
 * (`CoreDelegates.h:115`) — which is the literal next step `m6249889` asks for ("Name the flushed package").
 *
 * ⚠ WHY NOT `stat unit`. It shows a live number on screen and it steers nothing, which is fine as far as it
 * goes — but it does not write to `FactoryGame.log`, does not survive the session, cannot attribute a spike
 * to anything, and does not exist on a dedicated server, which has no viewport. The 560 ms save-serialisation
 * stall (`m6147432`) is a SERVER hitch; a client-only readout could never have found it. This meter arms on
 * both sides for that reason.
 *
 * VIEWER ONLY, like the overlay it posts to: it reads a clock and counts. It changes no cvar, writes no ini,
 * touches no vanilla state, and does no network I/O. `Side()` is `Any` and there is no authority question to
 * ask — a clock is a clock on both machines.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMHitchMeter final : public IFPMFix
{
public:
	static FFPMHitchMeter& Get();

	virtual const TCHAR* Name() const override { return TEXT("hitch-meter"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }
	virtual void Arm() override;
	virtual void Disarm() override;

	/**
	 * Re-primes the clock across a world load.
	 *
	 * Without this the first sample after a load is the whole loading screen, which is a real number about
	 * something nobody calls a hitch. The `FPM.Hitch.IgnoreAboveMs` ceiling would catch it anyway — this
	 * makes the intent explicit rather than leaning on a threshold to mean two different things.
	 */
	virtual void OnWorldLoad(UWorld* World) override;

	/** Print the running totals WITH their denominator. Bound to `FPM.Hitch.Report`, and run on disarm. */
	void LogSummary(const TCHAR* Reason);

private:
	/**
	 * The `float` this is handed is `FApp::GetDeltaTime()` and is DELIBERATELY UNUSED — see the header
	 * comment on smoothing. Named so that the next reader does not "fix" it by using the parameter.
	 */
	bool Tick(float SmoothedEngineDeltaDoNotUse);

	/**
	 * The single place a measured span is graded, so `Tick()` and `OnWorldLoad()` cannot grade it
	 * differently. Extracted 2026-08-09 when a review found that the world-load path graded it not at all.
	 *
	 * `bClosedByLoad` only affects what the log line SAYS. A span is a span; what closed it does not change
	 * how long the game thread was gone.
	 */
	void ClassifySpan(double SpanMs, int32 FlushesInSpan, bool bClosedByLoad);

	void OnAsyncLoadingFlush();
	void OnAsyncLoadPackage(FStringView PackageName);

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle FlushHandle;
	FDelegateHandle PackageHandle;

	double LastTickSeconds = 0.0;
	double WindowSeconds = 0.0;
	bool bPrimed = false;

	/** ★ THE DENOMINATOR. Every count below is meaningless without it, so it is never reported without it. */
	uint64 FramesInWindow = 0;
	uint64 FramesTotal = 0;

	int32 Hitches = 0;
	int32 HitchesWithFlush = 0;
	int32 LoadStalls = 0;

	/**
	 * ⚠ Stalls keep their flush count too, and the first version did NOT — a review found it and graded it
	 * HIGH. Dropping it meant the severe tail, where a synchronous load is most likely to BE the mechanism,
	 * silently fell out of the very statistic the meter exists to produce.
	 */
	int32 LoadStallsWithFlush = 0;
	int32 LinesThisWindow = 0;
	int32 LinesSuppressed = 0;
	double WorstHitchMs = 0.0;
	double HitchMsTotal = 0.0;

	int32 SessionHitches = 0;
	double SessionWorstMs = 0.0;

	/**
	 * Flushes are counted, not timed. `OnAsyncLoadingFlush` fires at the TOP of the flush
	 * (`AsyncLoading2.cpp:11171-11176`, before the `StartTime` the engine takes on the next line), so it
	 * marks a beginning with no matching end delegate to pair it with. The frame's own wall time already
	 * carries the duration; what the delegate adds is WHICH frame, and that is all it is asked for.
	 *
	 * Thread-safe because the async loader is not ours to make promises about. The broadcast site is on the
	 * game thread today; a counter costs nothing and removes the need to keep checking that it still is.
	 */
	FThreadSafeCounter FlushesInFrame;
	FThreadSafeCounter FlushesTotal;

	/** Guarded because `GetOnAsyncLoadPackage()` fires on whichever thread issued the load, by contract. */
	mutable FCriticalSection PackagesLock;
	TArray<FString> RecentPackages;

	/** Enough to name what was in flight, few enough that a hitch line stays one line. */
	static constexpr int32 MaxRecentPackages = 4;

	/** A pathological session must not turn the log into a wall. The overflow is reported, never hidden. */
	static constexpr int32 MaxLinesPerWindow = 20;
};
