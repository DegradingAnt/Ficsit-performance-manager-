// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeCounter.h"

#include <atomic>

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

	/** UnknownCause: a pure instrument. It handles no symptom and claims no cause -- it exists to NAME one, and as of
	 * 2026-08-09 the majority of measured hitches remain unattributed by it. That share is now PRINTED as a
	 * percentage rather than left implicit, so the next measurement can say whether it moved. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Hitch; }
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

	/**
	 * Print every synchronously-loaded package this session, most frequent first. Bound to
	 * `FPM.Hitch.Packages`.
	 *
	 * ★ THIS IS THE LIST A PIN SET MUST BE CHOSEN FROM. The per-hitch line names only the LAST sync load
	 * of its span, and one measured span contained 283 of them — so ranking by what appears in hitch lines
	 * ranks by coincidence. The asset-residency fix's own scope rule says to add an asset only when a
	 * blocking load of it has been shown in a log; this is the honest form of that evidence.
	 */
	void LogSyncPackages();

	/**
	 * `FPM.Pso.Report` — the whole PSO picture, including whether each bucket is CAPABLE of firing.
	 *
	 * ★ IT LEADS WITH THE CAPABILITY LINE, and that is the point of having a separate command. Three of
	 * the four inputs here are gated by console variables we do not own, so a zero from any of them has
	 * two readings — "no PSO work happened" and "this could never have reported anything". Printing
	 * `r.ShaderPipelineCache.Enabled` / `.ReportPSO` and `IsPSOPrecachingEnabled()` beside the counts is
	 * the liveness proof `FPMCVarWriter` sets the pattern for: prove the instrument can move before
	 * anyone reads a zero off it.
	 */
	void LogPsoReport();

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
	void ClassifySpan(double SpanMs, int32 FlushesInSpan, bool bClosedByLoad, int32 SyncLoadsInSpan,
	                  int32 GcInSpan);

	void OnAsyncLoadingFlush();
	void OnAsyncLoadPackage(FStringView PackageName);

	/**
	 * ★ THE HALF THE FLUSH DELEGATE CANNOT SEE — added 2026-08-09 after the 0.4.0 boot, and it is the
	 * fourth instrument gap of the run, this one in my own instrument.
	 *
	 * `FCoreDelegates::OnAsyncLoadingFlush` is broadcast ONLY when
	 * `ThreadContext.SyncLoadUsingAsyncLoaderCount == 0` (`AsyncLoading2.cpp:11171-11176`); the engine's
	 * own comment there says *"if the sync count is 0, then this flush is not triggered from a sync
	 * load"*. A `LoadAsset_Blocking` stall IS a sync load, so it never fires that delegate — meaning the
	 * flush counter was structurally blind to the exact category `m6249889` was opened about, and its
	 * `0 of them had an async-load flush` was a PARTIAL answer wearing a complete one's clothes.
	 *
	 * `OnSyncLoadPackage` (`CoreDelegates.h:117`, broadcast `UObjectGlobals.cpp:1742` / `:1815`) covers
	 * that half — and unlike the flush delegate it is handed the PACKAGE NAME, which is `m6249889`'s
	 * literal next step ("Name the flushed package") available at level 1 rather than only at verbose.
	 */
	void OnSyncLoadPackage(const FString& PackageName);

	/**
	 * ★ GC IS THE STANDING CANDIDATE FOR THE HITCHES NOTHING ELSE EXPLAINS — wired 2026-08-09.
	 *
	 * The 0.4.0 boot measured 92 client hitches (median 67.3 ms, p90 366.3 ms, max 972.8 ms) of which only
	 * 33 had a swapchain resize before them. Something accounts for the rest, and `m6253024`'s design
	 * names the mechanism from engine bytes: GC is SKIPPED while async loading
	 * (`if (GPerformGCWhileAsyncLoading || !IsAsyncLoading())`, `UnrealEngine.cpp:2017`, with
	 * `GPerformGCWhileAsyncLoading = 0` at `:1664`), so with World Partition streaming churning while the
	 * player MOVES, a due pass is deferred and fires the instant streaming quiets. Every pass is
	 * stop-the-world; measured worst on 0.55.0 was 148.6 ms.
	 *
	 * ⚠ THAT IS A HYPOTHESIS AND THIS IS THE INSTRUMENT THAT DECIDES IT, so it counts and does not steer.
	 * `GetPreGarbageCollectDelegate()` / `GetPostGarbageCollect()` (`UObjectGlobals.h:3343`, `:3359`) are
	 * plain `FSimpleMulticastDelegate&` — nothing here forces, defers, paces or configures a collection.
	 * The old design's L4 pacing lever is deliberately NOT built: it needs a cvar-writing surface FPM2
	 * does not have, and it must not be chosen before this measurement exists.
	 */
	void OnPreGarbageCollect();

	/**
	 * ★ PSO PRECOMPILE — A RUN-LEVEL SIGNAL, AND THE WHOLE POINT IS THAT IT IS NOT PRETENDING TO BE A
	 * PER-FRAME ONE. Wired 2026-08-09 as the fourth bucket.
	 *
	 * `FShaderPipelineCache::GetPrecompilationBeginDelegate()` / `GetPrecompilationCompleteDelegate()`
	 * (`ShaderPipelineCache.h:214`, `:219`) bracket an entire precompile RUN, which is batched across many
	 * frames by `r.ShaderPipelineCache.BatchSize`/`BatchTime`. So the flag they maintain is true for a long
	 * stretch — during startup, potentially minutes — and a naive "this hitch overlapped a PSO run" count
	 * would mark nearly every hitch in that stretch and mean nothing.
	 *
	 * ⚠ THAT IS WHY `FramesDuringPso` EXISTS. The bucket is reported as a RATE against its own denominator:
	 * hitches-per-span inside the run versus outside it. An elevated rate inside is evidence; a raw overlap
	 * count is not. This is the same denominator discipline the header opens with, applied to the one bucket
	 * that would otherwise be free to look impressive.
	 *
	 * ⚠ THE TWO THINGS I FIRST GOT WRONG HERE, both settled from engine bytes rather than from the plan:
	 *   1. `FShaderPipelineCache : public FTickableObjectRenderThread` (`ShaderPipelineCache.h:78`), and the
	 *      Complete broadcast is in its Tick (`ShaderPipelineCache.cpp:1816`). These fire on the RENDER
	 *      THREAD. The flag is `std::atomic` because it has to be, not as a precaution.
	 *   2. The Complete delegate's `Seconds` is NOT this run's stall duration. It is
	 *      `FShaderPipelineCacheTask::TotalPrecompileTime`, a `std::atomic_int64_t` accumulated with
	 *      `+= TimeDelta` per completed task (`:1204`) and reset after the broadcast (`:1826-1827`) — a sum
	 *      of compile time across the run, unattributable to any frame. It is recorded as session context
	 *      and deliberately kept OUT of the per-span attribution.
	 *
	 * The polling alternative was rejected on inspection, not on taste: `NumPrecompilesRemaining()` returns a
	 * wall-time DECAY ESTIMATE rather than a task count whenever `r.PSOFileCache.MaxPrecompileTime > 0`
	 * (`:831-835`), and is floored at 1 while any cache is pending (`:849-850`). Its per-frame delta would be
	 * a number that looks like work done and is not.
	 *
	 * On a dedicated server this simply never fires — no RHI means no `ShaderPipelineCache`, so the flag
	 * stays false and the bucket reads as an honest zero rather than needing a `Side()` exception.
	 *
	 * ⚠⚠ AND MEASUREMENT HAS NOW SHOWN THIS BUCKET IS A TAUTOLOGY IN-GAME. From Ant's 0.8.0 client log,
	 * 2026-08-10, primary:
	 *     07:04:58  FShaderPipelineCache starting pipeline cache 'FactoryGame' … 82 tasks
	 *     07:04:59  FShaderPipelineCache starting pipeline cache 'FactoryGame_usr' … 5454 tasks
	 *     07:05:19  FShaderPipelineCache FactoryGame_usr completed 5454 tasks in 0.74s
	 *     07:05:20  BeginNextPrecompileCacheTask() - Finished, no jobs remaining.
	 *     07:06:16  [FPM] hitch meter: running | … 0 were during a PSO precompile
	 * The precompile runs finish BEFORE the first hitch window opens. `bPsoRunActive` is therefore false
	 * for the whole playable session, and `0 were during a PSO precompile` is not the finding "PSO is
	 * innocent" — it is "this bucket cannot fire". Every window in every log says 0 for that reason.
	 * Kept anyway, because it correctly describes startup and its zero is now EXPLAINED rather than bare.
	 */
	void OnPsoPrecompileBegin(uint32 Count);
	void OnPsoPrecompileComplete(uint32 Count, double Seconds);

	/**
	 * ★ THE COLD-CREATION BUCKET — the PSO widening Ant asked for, and the half that was missing entirely.
	 * Wired 2026-08-10.
	 *
	 * Ant: *"Pso stuff need to be even wider"*. The run-level bucket above answers "was a precompile batch
	 * running", which after the 21-second startup is always no. It never had a chance at the thing she
	 * actually reports — *"hitches when moving fast through the world"*.
	 *
	 * ⚠ THE ENGINE ALREADY COUNTS THE REAL EVENT AND IT IS NOT SMALL. `LogPSOHitching`, from her own
	 * 03:27 client log:
	 *     Encountered  50 PSO creation hitches so far (35 graphics, 15 compute).  6 of them were precached.
	 *     Encountered 100 PSO creation hitches so far (79 graphics, 21 compute). 10 of them were precached.
	 * That is 100+ runtime pipeline creations each over `r.PSO.RuntimeCreationHitchThreshold` (20 ms) in
	 * about eleven minutes, and 90 of them were cold misses. The counters behind that line
	 * (`GraphicsPSOCreationHitchCount`, `PipelineStateCache.cpp:225-227`) are file-scope statics with no
	 * getter, and the matching `STAT_RuntimeGraphicsPSOHitchCount` is compiled out with `STATS` in
	 * Shipping. Neither is reachable from a mod. So the count is NOT what we subscribe to — the CAUSE is.
	 *
	 * ★ WHAT WE SUBSCRIBE TO, AND THE FULL EMITTER CHAIN, verified from engine bytes rather than assumed
	 * (a delegate that cannot fire is this project's most expensive defect class):
	 *   1. `FPipelineFileCacheManager::CacheGraphicsPSO` reaches `if (bActuallyNewPSO)` — the pipeline was
	 *      not already in the cache, i.e. a genuine cold creation (`PipelineFileCache.cpp:3608`).
	 *   2. `if (ReportNewPSOs())` → `NewPSOsToReport.Enqueue(NewEntry)` (`:3621-3623`).
	 *   3. `PipelineStateCache::FlushResources()`, which runs once per frame from
	 *      `RHICommandList::BeginFrame`, calls `BroadcastNewPSOsDelegate()` (`PipelineStateCache.cpp:3306`).
	 *   4. That drains the queue and, still gated on `ReportNewPSOs()`, marshals to the game thread via
	 *      `ExecuteOnGameThread` and broadcasts one event per PSO (`PipelineFileCache.cpp:3546-3556`).
	 *
	 * ★ AND THE GATE IS OPEN IN THE SHIPPED GAME, which is the part that had to be checked rather than
	 * hoped for. `ReportNewPSOs()` reads `r.ShaderPipelineCache.ReportPSO`, whose default is
	 * `PIPELINE_CACHE_DEFAULT_ENABLED` = `(!WITH_EDITOR)` (`PipelineFileCache.h:18`) — so **1** in a retail
	 * client. Corroborated on disk rather than from the default alone: her
	 * `Saved/FactoryGame_PCD3D_SM6.upipelinecache` is 2.6 MB, written 2026-08-10 05:33, and the log reports
	 * that user cache holding 5464 PSOs. The recording path is demonstrably running.
	 *
	 * ⚠ TWO HONEST LIMITS, stated because the rest of this file would be worthless if this one overclaimed:
	 *   1. **Attribution is ±1 frame.** The creation happens on the render thread; the broadcast is
	 *      marshalled to the game thread, so it can land in the next span. At a 50 ms threshold against
	 *      ~4 ms frames that is a real edge, and it is why the line says "created in this span" rather
	 *      than "caused by".
	 *   2. **A cold creation is not automatically a hitch.** The engine applies a 20 ms threshold before
	 *      calling one a hitch; we apply OUR threshold to the frame and report the coincidence. Broader
	 *      than the engine's counter on purpose — a cheap creation that lands inside a slow frame is still
	 *      the fact worth having.
	 *
	 * `int32` rather than the real `FPipelineCacheFileFormatPSO::DescriptorType`, for the same reason the
	 * two precompile callbacks take plain scalars: keeping `PipelineFileCache.h` out of FPM's public
	 * header. The lambda in `Arm()` unpacks `PSO.Type` (`Compute=0, Graphics=1, RayTracing=2`,
	 * `PipelineFileCache.h:202-207`). The split exists so the numbers are directly comparable against the
	 * `LogPSOHitching` line's own graphics/compute split.
	 */
	void OnPsoCreated(int32 DescriptorType);

	/**
	 * ★ WORK OR WAIT — the cut that halves the search space for every unattributed hitch.
	 * Wired 2026-08-10, and it is the answer to the shape her logs keep showing:
	 *     2 hitch(es) ... worst 212.5 ms ... 0 flush, 0 SYNC load, 0 GC, 0 PSO | 2 UNATTRIBUTED (100%)
	 *
	 * ⚠ EVERY BUCKET SO FAR ASKS "WHAT HAPPENED DURING THE SPAN", AND NONE ASKS THE PRIOR QUESTION.
	 * This meter times wall clock between core-ticker ticks. That span contains the game thread's own
	 * work AND everything it then waits on — frame-end sync, vsync, the render thread catching up. So a
	 * 723 ms hitch has two completely different explanations that the meter cannot currently tell apart:
	 * the game thread DID 723 ms of work, or the game thread did 4 ms of work and WAITED 719 ms.
	 * Those point at opposite halves of the engine, and chasing the wrong one is how the four
	 * instrument-gap incidents in this project's history started.
	 *
	 * ★ HOW THE SPLIT IS TAKEN, and why the ordering makes it valid. All four are plain CORE_API
	 * `FSimpleMulticastDelegate`s broadcast from the main engine loop with no editor guard:
	 *     OnBeginFrame    CoreDelegates.h:262   broadcast LaunchEngineLoop.cpp:5462   game thread
	 *     OnEndFrame      CoreDelegates.h:268   broadcast LaunchEngineLoop.cpp:5869   game thread
	 *     OnBeginFrameRT  CoreDelegates.h:271   broadcast LaunchEngineLoop.cpp:5283   render thread
	 *     OnEndFrameRT    CoreDelegates.h:274   broadcast LaunchEngineLoop.cpp:5318   render thread
	 * The core ticker that drives `Tick()` runs at `LaunchEngineLoop.cpp:5852` — BETWEEN OnBeginFrame and
	 * OnEndFrame. So `OnBeginFrame -> OnEndFrame` is the game thread's own frame work, measured on the
	 * same clock as the span, and `SpanMs - GameThreadBusyMs` is everything the game thread was not doing
	 * itself. That subtraction is only meaningful because of that ordering, which is why it is cited.
	 *
	 * The result is a three-way verdict per hitch:
	 *   - game thread busy for most of the span      -> GAME-THREAD BOUND. Look at gameplay, GC, loading.
	 *   - game thread idle, render thread busy       -> RENDER-THREAD BOUND. Look at draw calls, PSOs.
	 *   - both idle                                  -> neither. GPU, vsync, driver, or the OS.
	 * The third is a real answer too, and it is the one no existing bucket could ever have produced.
	 *
	 * ⚠ MICROSECONDS IN AN INTEGER ATOMIC, NOT `std::atomic<double>`. The render-thread pair accumulates
	 * across threads, and `fetch_add` on a floating-point atomic is a C++20 addition with uneven support.
	 * An int64 of microseconds is exact for these magnitudes and needs no compare-exchange loop.
	 */
	void OnFrameBeginGameThread();
	void OnFrameEndGameThread();
	void OnFrameBeginRenderThread();
	void OnFrameEndRenderThread();

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle FlushHandle;
	FDelegateHandle PackageHandle;
	FDelegateHandle SyncLoadHandle;
	FDelegateHandle PreGcHandle;
	FDelegateHandle PsoBeginHandle;
	FDelegateHandle PsoCompleteHandle;
	FDelegateHandle PsoLoggedHandle;
	FDelegateHandle FrameBeginHandle;
	FDelegateHandle FrameEndHandle;
	FDelegateHandle FrameBeginRtHandle;
	FDelegateHandle FrameEndRtHandle;

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

	/** The sync-load half. Counted separately because conflating them would hide which one fired. */
	FThreadSafeCounter SyncLoadsInFrame;
	FThreadSafeCounter SyncLoadsTotal;
	int32 HitchesWithSyncLoad = 0;
	int32 StallsWithSyncLoad = 0;

	/** GC passes are game-thread by construction here, but the counter costs nothing and asks no questions. */
	FThreadSafeCounter GcInFrame;
	FThreadSafeCounter GcTotal;
	int32 HitchesWithGc = 0;
	int32 StallsWithGc = 0;

	/**
	 * The PSO run flag, and — the part that makes it worth having — its own denominator. See the long note
	 * on `OnPsoPrecompileBegin` above for why a run-level flag needs one and the other three buckets do not:
	 * a flush, a sync load and a GC pass are EVENTS inside a span, this is a LEVEL that spans many.
	 *
	 * Read, never consumed, so `ClassifySpan` reads it directly instead of taking a sixth parameter. That
	 * also leaves both of its call sites untouched, which is the smaller change and the smaller risk.
	 */
	std::atomic<bool> bPsoRunActive{false};
	int32 FramesDuringPso = 0;
	int32 HitchesWithPso = 0;
	int32 StallsWithPso = 0;

	/**
	 * Cold PSO creations — an EVENT per newly-created pipeline, unlike the run flag above. See the long
	 * note on `OnPsoCreated`. Consumed inside `ClassifySpan` rather than at its two call sites: the
	 * signature already carries a `bool` wedged between two `int32`s, and a sixth positional scalar there
	 * is a silent-swap bug waiting to happen. Consuming inside also makes it impossible for a future call
	 * site to forget the reset, which is the failure the call-site pattern is exposed to.
	 *
	 * Thread-safe because the broadcast reaches us through `ExecuteOnGameThread` and a counter costs
	 * nothing — the same reasoning already applied to the flush and sync-load counters.
	 */
	FThreadSafeCounter PsoCreatesInFrame;
	FThreadSafeCounter PsoCreatesTotal;

	/** Split to match the `LogPSOHitching` line's own graphics/compute split, so the two are comparable. */
	FThreadSafeCounter PsoCreatesGraphics;
	FThreadSafeCounter PsoCreatesCompute;
	FThreadSafeCounter PsoCreatesRayTracing;

	/**
	 * ★ PSOs CREATED AFTER THE WORLD SETTLED — the number that decides the pre-optimize question.
	 *
	 * Ant deferred the `r.ShaderPipelineCache.PreOptimizeEnabled` ini exception on 2026-08-10 pending a
	 * startup measurement, and this is it. Pre-optimize can only help PSOs that would otherwise compile
	 * during PLAY; `PsoCreatesTotal` is dominated by the startup burst it would merely move. See the
	 * long note on `OnPsoCreated` for how to read a zero here — it is a real answer, not an absent one.
	 */
	FThreadSafeCounter PsoCreatesAfterSettle;

	/**
	 * Wall-clock second after which a PSO creation counts as mid-play. Set on every world load, zero
	 * before the first one — which is what keeps the main menu's own PSOs out of the gameplay count.
	 *
	 * ⚠ ATOMIC BECAUSE `OnPsoCreated` IS NOT ON THE GAME THREAD. Every counter beside it is a
	 * `FThreadSafeCounter` for exactly that reason, and this is written from `OnWorldLoad` on the game
	 * thread while being read from whichever thread built the pipeline. Relaxed ordering is enough: it
	 * is a threshold compared against a clock, so reading the previous world's value for one PSO
	 * misfiles at most that one sample and cannot corrupt anything.
	 */
	std::atomic<double> SettledRealSeconds{0.0};

	int32 HitchesWithPsoCreate = 0;
	int32 StallsWithPsoCreate = 0;

	/**
	 * ★ ASYNC PSO WORK IN FLIGHT — the third mechanism, and the second one the old bucket was blind to.
	 *
	 * UE 5.6 has THREE ways a pipeline costs time, and FPM watched exactly one:
	 *   1. bundled pipeline-cache precompile — `bPsoRunActive`, above. Startup only, measured.
	 *   2. **PSO precaching** (`r.PSOPrecache`) — `PipelineStateCache::NumActivePrecacheRequests()`.
	 *      Entirely invisible until now, and in 5.6 it is the mechanism doing most of the work.
	 *   3. on-demand cold creation at draw time — the `OnPsoCreated` bucket above.
	 *
	 * Both levels are sampled per span. `GetNumActivePipelinePrecompileTasks()` is one atomic load
	 * (`PipelineStateCache.cpp:2908-2911`) and `NumActivePrecacheRequests()` is two atomic reads with no
	 * lock on any branch (`:2318-2328`) — checked before wiring them into a per-frame path, because this
	 * is a performance mod and a per-frame lock would be the joke writing itself.
	 *
	 * Reported as a RATE against `FramesDuringPsoWork`, for the reason the run bucket already documents:
	 * a level that spans many frames needs its own denominator or the raw overlap count flatters itself.
	 */
	int32 FramesDuringPsoWork = 0;
	int32 HitchesWithPsoWork = 0;
	int32 StallsWithPsoWork = 0;
	int32 PeakPrecompileTasks = 0;
	int32 PeakPrecacheRequests = 0;

	/**
	 * The work-or-wait split. See the note on `OnFrameBeginGameThread`.
	 *
	 * The game-thread pair is touched only from the game thread, so the start stamp is a plain double.
	 * Its accumulator is still atomic because `ClassifySpan` consumes it with an exchange and there is no
	 * reason to have two consumption idioms in one file.
	 */
	double GtFrameStartSeconds = 0.0;

	/** The render-thread pair. Both are written from the render thread, so both must be atomic. */
	std::atomic<int64> RtFrameStartCycles{0};
	std::atomic<int64> RtBusyUsInSpan{0};

	/**
	 * ★ THE SPLIT'S OWN LIVENESS PROOF. Without it the three-way verdict's fall-through reports
	 * "NEITHER THREAD BUSY - gpu/vsync/os" for every hitch when the four frame delegates are simply not
	 * firing — a specific and confident wrong cause, which is strictly worse than a dead zero. A zero is
	 * useless; that actively certifies the wrong subsystem.
	 *
	 * Counting frames answers the question the review demands of every counter here: what input makes
	 * this non-zero? One `OnEndFrame` broadcast. If that never happens the meter says the split is
	 * unavailable instead of inventing a verdict.
	 */
	std::atomic<int64> GtFramesSeen{0};
	std::atomic<int64> RtFramesSeen{0};

	/**
	 * ⚠ THE VERDICT COUNTERS, and the thresholds they use are deliberately generous rather than tuned.
	 * A hitch is called game-thread bound when the game thread's own frame work covers most of the span,
	 * render-thread bound when it does not and the render thread's does. Anything else is left in the
	 * third bucket rather than forced into one of the first two — an honest "neither" is the finding for
	 * a GPU or vsync stall, and rounding it into a named bucket would be the overstatement this whole
	 * file argues against.
	 */
	int32 HitchesGameThreadBound = 0;
	int32 HitchesRenderThreadBound = 0;
	int32 HitchesNeitherThreadBusy = 0;

	/** Hitches measured while the split could not speak. Kept apart from the three real verdicts so a
	 *  broken instrument can never be mistaken for a finding about the GPU. */
	int32 HitchesSplitUnavailable = 0;
	double WorstGtBusyMs = 0.0;
	double WorstRtBusyMs = 0.0;

	/**
	 * ★ THE UNATTRIBUTED RATE IS THIS WIDENING'S DONE-CONDITION, not a by-product of it.
	 *
	 * Ant's 0.6.0 overlay read `21 hitch(es) ... 0 async-load flush, 0 SYNC load, 0 GC` — every hitch outside
	 * every bucket. Adding a fifth bucket LOOKS like progress whether or not the anonymous share actually
	 * falls, so the share itself is printed. It is the number that has to move, and it is the number that
	 * makes the next bucket's value arguable in advance instead of in hindsight.
	 */
	int32 HitchesUnattributed = 0;
	int32 StallsUnattributed = 0;

	/**
	 * Session context from the Complete delegate. Atomic because it is written on the render thread and read
	 * on the game thread in `LogSummary` — a torn read here would be a real race, not a theoretical one.
	 * `PsoSecondsLastRun` is the run's TOTAL compile time, reported as exactly that and nothing more.
	 */
	std::atomic<int32>  PsoRunsCompleted{0};
	std::atomic<uint32> PsoTasksLastRun{0};
	std::atomic<double> PsoSecondsLastRun{0.0};

	/** The most recent sync-loaded package name, so a hitch line can NAME what blocked it. */
	mutable FCriticalSection SyncNameLock;
	FString LastSyncPackage;

	/**
	 * ★ HOW OFTEN EACH PACKAGE BLOCKS — because `last=` is a BIASED SAMPLE and acting on it would be
	 * choosing by an artefact of the reporting. One measured span held 283 sync loads and named one.
	 * Guarded by SyncNameLock, which already covers this callback.
	 */
	TMap<FString, int32> SyncLoadCounts;

	/** Guarded because `GetOnAsyncLoadPackage()` fires on whichever thread issued the load, by contract. */
	mutable FCriticalSection PackagesLock;
	TArray<FString> RecentPackages;

	/** Enough to name what was in flight, few enough that a hitch line stays one line. */
	static constexpr int32 MaxRecentPackages = 4;

	/** A pathological session must not turn the log into a wall. The overflow is reported, never hidden. */
	static constexpr int32 MaxLinesPerWindow = 20;
};
