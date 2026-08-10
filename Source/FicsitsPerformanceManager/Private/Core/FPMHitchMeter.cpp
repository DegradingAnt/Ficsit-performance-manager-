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

// The PSO widening's two engine surfaces. Both are RHI, which is already a PRIVATE dependency of this
// module (added for RHIGetTextureMemoryStats) — and both headers are included only from Private/, so the
// existing private entry is exactly right and Build.cs needs no change.
#include "PipelineFileCache.h"
#include "PipelineStateCache.h"

// FSelfRegisteringExec::StaticExec, for the engine PSO-hitch log switch below.
#include "Misc/CoreMisc.h"

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

/*
 * ★ THE ENGINE ALREADY MEASURES EXACTLY WHAT WE WANT AND SIMPLY DOES NOT PRINT IT.
 *
 * Ant, 2026-08-10: *"Fpm should hook whatever it needs. We need all the control we can get"*. Taking that
 * as permission rather than as an instruction to hook first, because a cheaper route turned out to carry
 * MORE data than the hook would have.
 *
 * `PipelineStateCache.cpp:238-279` times every runtime pipeline creation and, past
 * `r.PSO.RuntimeCreationHitchThreshold` (20 ms), logs:
 *
 *     UE_LOG(LogPSOHitching, Verbose, TEXT("Runtime graphics PSO creation hitch (%.2f msec) for %s "
 *            "(precache status: %s)"), ...)
 *
 * Duration, the pipeline's name, and whether it had been precached — per hitch. That is strictly more
 * than a hook on `FDynamicRHI::RHICreateGraphicsPipelineState` could produce, and it costs no hook at all.
 *
 * ★ AND IT IS COMPILED INTO THE SHIPPED BINARY, which is the fact that had to be checked rather than
 * hoped for. Verbose survives here for two reasons together:
 *   - `USE_LOGGING_IN_SHIPPING 1` in this build's own SharedDefinitions header, so `NO_LOGGING` is 0
 *     (`Misc/Build.h:320`).
 *   - `COMPILED_IN_MINIMUM_VERBOSITY` defaults to `VeryVerbose` (`LogMacros.h:81-82`) and may only be
 *     overridden in a monolithic build. Nothing in this project's shipping definitions overrides it.
 * So the line exists in the binary and is suppressed only at RUNTIME, by the category's declared default
 * of `Log`. One runtime switch reveals it.
 *
 * ⚠ WHY THIS IS NOT THE HOOK, and when it should become one. A hook would give per-creation timing that
 * FPM owns and can attribute in-game on the overlay. This gives a richer line in the log and nothing on
 * screen. The in-game half is already covered by the cold-creation counter, so the hook buys only
 * millisecond attribution on the overlay — and it would sit on the render thread inside the renderer's
 * pipeline creation path. That is worth doing when a boot shows the log half is not enough, and not
 * before. Evidence first is this file's whole argument.
 *
 * ZERO RESIDUE: a log category's runtime verbosity is in-memory only. Nothing is written to any ini, and
 * `Disarm()` puts the category back to its declared `Log` default.
 */
static TAutoConsoleVariable<int32> CVarPsoEngineHitchLog(
	TEXT("FPM.Pso.EngineHitchLog"), 1,
	TEXT("1 raises the engine's own LogPSOHitching category to Verbose, which prints one line per runtime "
	     "pipeline creation over r.PSO.RuntimeCreationHitchThreshold ms with its duration, name and "
	     "precache status. 0 leaves the category alone. Restored on unload either way. Default 1."),
	ECVF_Default);

/**
 * Flip the engine's PSO-hitch category. `FSelfRegisteringExec::StaticExec` is CORE_API and the `LOG`
 * command is handled by `FLogSuppressionImplementation::Exec_Runtime` (`LogSuppressionInterface.cpp:589`,
 * `:591`) — `Exec_Runtime` rather than `Exec_Dev`, so it is present in Shipping.
 *
 * By name, not by symbol, and that is forced rather than chosen: the category is
 * `DEFINE_LOG_CATEGORY_STATIC` inside `PipelineStateCache.cpp`, so there is no linkable symbol to hand to
 * `UE_SET_LOG_VERBOSITY`. Categories register themselves by name at construction, so the name route
 * reaches it regardless of static linkage.
 */
static void FPMSetEnginePsoHitchLogging(bool bVerbose)
{
	if (GLog == nullptr) { return; }

	// `Log` is LogPSOHitching's own declared default (`PipelineStateCache.cpp:45`), so this restores
	// rather than guesses. `Log Reset` would have reset EVERY category, which is not ours to do.
	const TCHAR* Cmd = bVerbose
		? TEXT("Log LogPSOHitching Verbose")
		: TEXT("Log LogPSOHitching Log");

	FSelfRegisteringExec::StaticExec(nullptr, Cmd, *GLog);
}

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

	// The cold-creation bucket. A lambda for the same reason as the two above — it keeps
	// FPipelineCacheFileFormatPSO out of FPM's public header; the member below takes a plain int32.
	PsoLoggedHandle = FPipelineFileCacheManager::OnPipelineStateLogged().AddLambda(
		[this](const FPipelineCacheFileFormatPSO& PSO)
		{ OnPsoCreated(static_cast<int32>(PSO.Type)); });

	// The work-or-wait split. All four are plain multicast delegates from the main engine loop.
	FrameBeginHandle   = FCoreDelegates::OnBeginFrame.AddRaw(this, &FFPMHitchMeter::OnFrameBeginGameThread);
	FrameEndHandle     = FCoreDelegates::OnEndFrame.AddRaw(this, &FFPMHitchMeter::OnFrameEndGameThread);
	FrameBeginRtHandle = FCoreDelegates::OnBeginFrameRT.AddRaw(this, &FFPMHitchMeter::OnFrameBeginRenderThread);
	FrameEndRtHandle   = FCoreDelegates::OnEndFrameRT.AddRaw(this, &FFPMHitchMeter::OnFrameEndRenderThread);

	// Reveal the engine's own per-creation timing line. See the long note above the cvar.
	// ⚠ Never on a dedicated server: NullRHI builds no pipelines, so this would raise a category that
	// cannot emit and put a misleading switch in the server log for nothing.
	if (!IsRunningDedicatedServer() && CVarPsoEngineHitchLog.GetValueOnAnyThread() != 0)
	{
		FPMSetEnginePsoHitchLogging(true);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] engine PSO-hitch logging raised to Verbose. The game will now print one "
			     "'Runtime graphics/compute PSO creation hitch (N msec)' line per pipeline built above "
			     "r.PSO.RuntimeCreationHitchThreshold. Set FPM.Pso.EngineHitchLog 0 to leave it alone."));
	}

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
	if (PsoLoggedHandle.IsValid())
	{
		FPipelineFileCacheManager::OnPipelineStateLogged().Remove(PsoLoggedHandle);
		PsoLoggedHandle.Reset();
	}

	/*
	 * ⚠ THE RENDER-THREAD PAIR COMES OFF FROM THE GAME THREAD, AND THAT RACE IS ACCEPTED RATHER THAN
	 * CLOSED. Stated plainly because the alternative reading — that unbinding is synchronised — is wrong,
	 * and a comment that implies a guarantee the code does not provide is worse than no comment.
	 *
	 * `Remove()` on a multicast delegate is not ordered against a broadcast already running on the render
	 * thread, so `OnFrameEndRenderThread` can land immediately after this. Two reasons that is safe here
	 * and does not need a `FlushRenderingCommands()`:
	 *   - The meter is a function-local static with process lifetime (`Get()`), so a late callback cannot
	 *     touch freed memory. This is not a use-after-free.
	 *   - All it can do is `fetch_add` into `RtBusyUsInSpan`, which nothing reads after this point —
	 *     `LogSummary` already ran at the top of `Disarm()`.
	 * Flushing rendering commands during teardown, when the render thread may already be going away, is
	 * the riskier of the two options for a consequence that is provably nil.
	 */
	if (FrameBeginHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(FrameBeginHandle);
		FrameBeginHandle.Reset();
	}
	if (FrameEndHandle.IsValid())
	{
		FCoreDelegates::OnEndFrame.Remove(FrameEndHandle);
		FrameEndHandle.Reset();
	}
	if (FrameBeginRtHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrameRT.Remove(FrameBeginRtHandle);
		FrameBeginRtHandle.Reset();
	}
	if (FrameEndRtHandle.IsValid())
	{
		FCoreDelegates::OnEndFrameRT.Remove(FrameEndRtHandle);
		FrameEndRtHandle.Reset();
	}

	// ZERO RESIDUE. Restored unconditionally rather than behind the same cvar the arm path checked:
	// someone can set FPM.Pso.EngineHitchLog to 0 mid-session, and a restore that reads the CURRENT value
	// would then leave the category raised forever. Putting it back to its declared default is correct
	// whether or not we were the ones who moved it.
	if (!IsRunningDedicatedServer())
	{
		FPMSetEnginePsoHitchLogging(false);
	}
}

/*
 * ★ THE WORK-OR-WAIT PAIR. Four callbacks, no hook, no allocation, and nothing that can block a frame.
 *
 * `FPlatformTime::Seconds()` on the game-thread side, matching the span clock exactly so the subtraction
 * in `ClassifySpan` compares like with like. Cycles64 on the render-thread side, because that pair only
 * ever produces a DURATION and cycles avoid a second conversion on the hotter thread.
 */
void FFPMHitchMeter::OnFrameBeginGameThread()
{
	GtFrameStartSeconds = FPlatformTime::Seconds();
}

void FFPMHitchMeter::OnFrameEndGameThread()
{
	/*
	 * ⚠⚠ THIS DELIBERATELY DOES NOT ACCUMULATE, AND THE FIRST VERSION DID — review blocker, 2026-08-10.
	 *
	 * Accumulating here and consuming at tick time is off by exactly one frame, because of where our
	 * ticker sits inside the engine loop:
	 *     LaunchEngineLoop.cpp:5462   OnBeginFrame.Broadcast()
	 *     LaunchEngineLoop.cpp:5852   FTSTicker::GetCoreTicker().Tick()   <- Tick() and ClassifySpan
	 *     LaunchEngineLoop.cpp:5869   OnEndFrame.Broadcast()              <- this callback
	 * The span closes BEFORE this frame's OnEndFrame runs, so an accumulator read at that point holds the
	 * PREVIOUS frame's duration while `SpanMs` measures the current one.
	 *
	 * The consequence was not a small inaccuracy, it was a confident wrong answer: a 700 ms game-thread
	 * stall in frame N gave SpanMs≈700 with GtBusy≈4 (frame N-1), which failed both verdict tests and
	 * printed "NEITHER THREAD BUSY - gpu/vsync/os". The single hitch class the split exists to name was
	 * the one it misnamed, and it pointed at the wrong half of the engine.
	 *
	 * `ClassifySpan` now reads `FPlatformTime::Seconds() - GtFrameStartSeconds` directly, which is the
	 * CURRENT frame's game-thread work up to the tick, on the same clock as the span. All this callback
	 * has to do is close the frame so a span that lands between frames measures nothing rather than
	 * measuring stale work.
	 */
	GtFrameStartSeconds = 0.0;
	GtFramesSeen.fetch_add(1, std::memory_order_relaxed);
}

void FFPMHitchMeter::OnFrameBeginRenderThread()
{
	RtFrameStartCycles.store(static_cast<int64>(FPlatformTime::Cycles64()), std::memory_order_relaxed);
}

void FFPMHitchMeter::OnFrameEndRenderThread()
{
	const int64 Start = RtFrameStartCycles.exchange(0, std::memory_order_relaxed);
	if (Start == 0) { return; }

	const double BusyMs = FPlatformTime::ToMilliseconds64(
		FPlatformTime::Cycles64() - static_cast<uint64>(Start));
	if (BusyMs <= 0.0) { return; }

	RtBusyUsInSpan.fetch_add(static_cast<int64>(BusyMs * 1000.0), std::memory_order_relaxed);
	RtFramesSeen.fetch_add(1, std::memory_order_relaxed);
}

void FFPMHitchMeter::OnPsoCreated(int32 DescriptorType)
{
	PsoCreatesInFrame.Increment();
	PsoCreatesTotal.Increment();

	// FPipelineCacheFileFormatPSO::DescriptorType — Compute=0, Graphics=1, RayTracing=2
	// (`PipelineFileCache.h:202-207`). Kept as a switch on the raw value rather than a cast back to the
	// enum, so this file needs no RHI type in its own signature.
	switch (DescriptorType)
	{
	case 0:  PsoCreatesCompute.Increment();    break;
	case 1:  PsoCreatesGraphics.Increment();   break;
	case 2:  PsoCreatesRayTracing.Increment(); break;
	default: break;
	}

	// No log line per PSO, deliberately. Her 03:27 session created enough of these to reach the engine's
	// own 100-hitch marker, and one line each would bury every other channel. The per-span count in the
	// hitch line and the session split in FPM.Pso.Report are what the question actually needs.
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

	/*
	 * ★ THE COLD-CREATION COUNT — consumed here, not at the call sites. Set() returns the old value and
	 * resets in one atomic operation, so a creation landing between the read and the reset cannot be lost.
	 * See the header note for why this one is consumed inside while the other three are parameters.
	 */
	const int32 PsoCreatesInSpan = PsoCreatesInFrame.Set(0);

	/*
	 * ★ THE TWO ASYNC LEVELS THE RUN FLAG CANNOT SEE. Sampled once per span; both are plain atomic reads.
	 *
	 * `GetNumActivePipelinePrecompileTasks()` is finer than `bPsoRunActive` by construction — the run flag
	 * is a single bool held true across a whole batched run, this is the task count in flight right now.
	 * `NumActivePrecacheRequests()` covers `r.PSOPrecache`, a mechanism this meter had no visibility into
	 * at all.
	 */
	const int32 PrecompileTasks  = PipelineStateCache::GetNumActivePipelinePrecompileTasks();
	const int32 PrecacheRequests = static_cast<int32>(PipelineStateCache::NumActivePrecacheRequests());
	const bool  bPsoWorkInSpan   = PrecompileTasks > 0 || PrecacheRequests > 0;

	PeakPrecompileTasks  = FMath::Max(PeakPrecompileTasks, PrecompileTasks);
	PeakPrecacheRequests = FMath::Max(PeakPrecacheRequests, PrecacheRequests);
	if (bPsoWorkInSpan) { ++FramesDuringPsoWork; }

	/*
	 * ★ WORK OR WAIT. Consumed every span, exactly like the event counters, so a span always reports its
	 * own frame time rather than a neighbour's. See the header note for why the subtraction is valid.
	 *
	 * ⚠ THIS IS ORTHOGONAL TO `bAttributed` AND MUST STAY SO. Attribution answers WHAT happened; this
	 * answers WHERE the time went. A hitch can be confidently game-thread bound and still completely
	 * unattributed — that pairing is not a contradiction, it is the most useful thing this meter can say
	 * about a hitch it cannot name: the cause is on the game thread and none of the four known causes
	 * explains it. Folding this into `bAttributed` would destroy that reading by making every hitch look
	 * explained.
	 */
	/*
	 * ★ THE GAME-THREAD SIDE IS READ LIVE, NOT ACCUMULATED. See the long note on `OnFrameEndGameThread`
	 * for the off-by-one this replaced. `GtFrameStartSeconds` is non-zero exactly when we are inside a
	 * frame, which — given the ticker sits at `LaunchEngineLoop.cpp:5852`, between OnBeginFrame and
	 * OnEndFrame — is the normal case for every span this meter closes.
	 *
	 * `SpanMs - GtBusyMs` is then the time the game thread was NOT doing its own frame work: the
	 * frame-end sync, the vsync wait, and the render thread catching up.
	 */
	const double GtBusyMs = GtFrameStartSeconds > 0.0
		? (FPlatformTime::Seconds() - GtFrameStartSeconds) * 1000.0
		: 0.0;

	/*
	 * The render-thread side stays accumulated, and that is correct rather than inconsistent: the render
	 * thread runs asynchronously and a frame it FINISHES during this span may have started in a previous
	 * one. This measures render-thread frame time completed within the span, which is the honest quantity
	 * available from these two delegates. It is not "the render thread's work on this frame".
	 */
	const double RtBusyMs = static_cast<double>(RtBusyUsInSpan.exchange(0, std::memory_order_relaxed)) / 1000.0;

	/*
	 * ★ THE LIVENESS PROOF FOR THE SPLIT — review blocker, 2026-08-10, and the more dangerous of the two.
	 *
	 * The three-way verdict below has a fall-through: anything that is neither game-thread bound nor
	 * render-thread bound is reported as "NEITHER THREAD BUSY - gpu/vsync/os". If the four frame
	 * delegates never fired, both numbers are 0.0 forever, every hitch fails both tests, and the meter
	 * reports a SPECIFIC AND CONFIDENT CAUSE that is a lie. That is worse than a dead zero — a zero is
	 * merely useless, this actively certifies the wrong subsystem.
	 *
	 * So the split declares its own liveness. If no game-thread frame has ever been seen, there is no
	 * verdict to give and the line says so instead of guessing.
	 */
	const bool bSplitLive = GtFramesSeen.load(std::memory_order_relaxed) > 0;

	/*
	 * ⚠ AND THE TWO HALVES ARE PROVED SEPARATELY, because on a dedicated server exactly one of them is
	 * alive. `Side()` is `Any`, the engine loop runs there, so OnBeginFrame/OnEndFrame DO fire — but
	 * there is no render thread, so OnEndFrameRT never does. Treating the pair as one liveness flag would
	 * make every server hitch that is not game-thread bound print "gpu/vsync/os" about a machine with no
	 * GPU. That is the same confident-wrong-cause failure the split flag above exists to prevent, one
	 * level down, and it would have shipped.
	 */
	const bool bRtLive = RtFramesSeen.load(std::memory_order_relaxed) > 0;

	// The point of the widening: a span that matched nothing is now counted rather than merely absent from
	// four other counters. Design `:1218` -- "most stalls were anonymous BY CONSTRUCTION".
	//
	// ⚠ THE TWO NEW TERMS EARN THEIR PLACE HERE FOR DIFFERENT REASONS. A cold PSO creation is an EVENT
	// inside the span and belongs beside the flush and sync-load terms. In-flight async PSO work is a
	// LEVEL, like the run flag, and is the weaker claim of the two -- so it is last, and the summary
	// reports it as a rate rather than a count.
	const bool bAttributed = FlushesInSpan > 0 || SyncLoadsInSpan > 0 || GcInSpan > 0 || bPsoInSpan
		|| PsoCreatesInSpan > 0 || bPsoWorkInSpan;

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
		if (FlushesInSpan > 0)    { ++LoadStallsWithFlush; }
		if (SyncLoadsInSpan > 0)  { ++StallsWithSyncLoad; }
		if (GcInSpan > 0)         { ++StallsWithGc; }
		if (bPsoInSpan)           { ++StallsWithPso; }
		if (PsoCreatesInSpan > 0) { ++StallsWithPsoCreate; }
		if (bPsoWorkInSpan)       { ++StallsWithPsoWork; }
		if (!bAttributed)         { ++StallsUnattributed; }
		return;
	}

	if (SpanMs < ThresholdMs) { return; }

	++Hitches;
	++SessionHitches;
	HitchMsTotal += SpanMs;
	WorstHitchMs   = FMath::Max(WorstHitchMs, SpanMs);
	SessionWorstMs = FMath::Max(SessionWorstMs, SpanMs);
	if (FlushesInSpan > 0)    { ++HitchesWithFlush; }
	if (SyncLoadsInSpan > 0)  { ++HitchesWithSyncLoad; }
	if (GcInSpan > 0)         { ++HitchesWithGc; }
	if (bPsoInSpan)           { ++HitchesWithPso; }
	if (PsoCreatesInSpan > 0) { ++HitchesWithPsoCreate; }
	if (bPsoWorkInSpan)       { ++HitchesWithPsoWork; }
	if (!bAttributed)         { ++HitchesUnattributed; }

	/*
	 * The three-way verdict. Half the span is the bar for "this thread was the one busy" — generous on
	 * purpose. A tuned threshold would imply a precision these two clocks do not have, and the useful
	 * signal here is coarse by nature: which HALF of the engine to go and look at.
	 *
	 * The `else` is a real verdict, not a leftover. Both threads idle across a 200 ms span means the time
	 * went somewhere neither of them owns — the GPU, the swapchain, vsync, the driver, or the OS taking
	 * the core away. No existing bucket in this meter could ever have said that.
	 */
	const bool bGtBound = bSplitLive && GtBusyMs >= SpanMs * 0.5;
	const bool bRtBound = bSplitLive && bRtLive && !bGtBound && RtBusyMs >= SpanMs * 0.5;
	if (!bSplitLive)   { ++HitchesSplitUnavailable; }
	else if (bGtBound) { ++HitchesGameThreadBound; }
	else if (bRtBound) { ++HitchesRenderThreadBound; }
	else               { ++HitchesNeitherThreadBusy; }

	// Only meaningful while the split is live, and only sampled on hitching spans — which is why the
	// summary calls it "worst seen ON A HITCH" rather than implying it saw every frame.
	if (bSplitLive)
	{
		WorstGtBusyMs = FMath::Max(WorstGtBusyMs, GtBusyMs);
		WorstRtBusyMs = FMath::Max(WorstRtBusyMs, RtBusyMs);
	}

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

	// ★ THE COLD-CREATION FIELD IS THE STRONGEST OF THE PSO THREE, so it is worded as a fact rather than
	// as an overlap: N pipelines that were not in the cache were built during this span. The ±1 frame
	// marshalling caveat is why it still says "in this span" and not "caused this".
	FString PsoCreatePart;
	if (PsoCreatesInSpan > 0)
	{
		PsoCreatePart = FString::Printf(TEXT(" | %d COLD PSO CREATION(S) in this span"), PsoCreatesInSpan);
	}

	// The weakest of the three, and worded to match: a level that was non-zero, with its numbers, and no
	// claim attached.
	FString PsoWorkPart;
	if (bPsoWorkInSpan)
	{
		PsoWorkPart = FString::Printf(TEXT(" | PSO work in flight: %d precompile task(s), %d precache request(s)"),
			PrecompileTasks, PrecacheRequests);
	}

	const TCHAR* UnattributedPart = bAttributed ? TEXT("") : TEXT(" | UNATTRIBUTED");

	// ★ THE FIRST FIELD ANYONE READING A HITCH LINE SHOULD LOOK AT, because it says which half of the
	// engine to open. Both raw numbers are printed beside the verdict so the verdict can be checked
	// rather than trusted.
	// The third verdict's WORDING depends on whether a render thread exists at all. On a dedicated server
	// it does not, so naming the GPU there would be nonsense about a machine that has none.
	const TCHAR* Verdict =
		bGtBound ? TEXT("GAME-THREAD BOUND")
		: bRtBound ? TEXT("RENDER-THREAD BOUND")
		: bRtLive ? TEXT("NEITHER THREAD BUSY - gpu/vsync/os")
		          : TEXT("GAME THREAD IDLE, no render thread on this side - blocked off-thread");

	const FString ThreadPart = bSplitLive
		? FString::Printf(
			TEXT(" | %s (game thread busy %.1f ms, render thread completed %.1f ms of %.1f ms)"),
			Verdict, GtBusyMs, RtBusyMs, SpanMs)
		: FString(TEXT(" | thread split UNAVAILABLE - no frame delegate has fired, so this hitch has NO "
		               "where-verdict. Do not read it as a GPU stall."));

	UE_LOG(LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] HITCH %.1f ms%s%s - %d async-load flush(es), %d GC pass(es) in this span, %d "
		     "package(s) still loading%s%s%s%s%s%s"),
		SpanMs, bClosedByLoad ? TEXT(" (closed by a world load)") : TEXT(""), *ThreadPart,
		FlushesInSpan, GcInSpan, GetNumAsyncPackages(), *SyncPart, *PsoCreatePart, *PsoWorkPart, PsoPart,
		UnattributedPart, *Packages);
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

		/*
		 * ⚠ ALL FOUR IN-FRAME COUNTERS ARE DISCARDED HERE, NOT JUST FLUSHES — corrected 2026-08-10 while
		 * adding the PSO one, and the first three were a pre-existing bug of the same shape.
		 *
		 * The stated intent was always "attributing a loading screen's flushes to the first playable frame
		 * would manufacture exactly the correlation this meter exists to test honestly". That reasoning
		 * covers sync loads and GC passes word for word, and they were left accumulating anyway — so the
		 * first span after every load carried the whole loading screen's sync-load count.
		 *
		 * It matters most for the new one. A loading screen builds a large number of cold pipelines, the
		 * first playable span is a common place for a hitch, and the two together would have produced a
		 * confident false attribution in the very bucket this widening was built to make trustworthy.
		 */
		FlushesInFrame.Set(0);
		SyncLoadsInFrame.Set(0);
		GcInFrame.Set(0);
		PsoCreatesInFrame.Set(0);

		// The work-or-wait accumulators too, and an in-flight frame start with them. A loading screen's
		// game-thread time folded into the first playable span would make it read as game-thread bound no
		// matter what actually happened in it.
		// ⚠ `GtFrameStartSeconds` is NOT cleared here, unlike everything else in this block. It is read
		// live rather than accumulated, so it holds the CURRENT frame's start — the frame we are standing
		// in right now, since the ticker runs between OnBeginFrame and OnEndFrame. Clearing it would blind
		// the very next span for no benefit. The render-thread accumulator IS cleared, because it carries
		// work from before the load boundary.
		RtBusyUsInSpan.store(0, std::memory_order_relaxed);
		RtFrameStartCycles.store(0, std::memory_order_relaxed);
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

	/*
	 * ★ THREE ROWS, NOT ONE — and the reason is a screenshot, 2026-08-10.
	 *
	 * Every widening this session appended another field to a single summary string. By 0.8.4 it had
	 * reached roughly six hundred characters, and Ant's overlay screenshot showed it spilling off BOTH
	 * edges of a 1080p screen, unreadable. The log can carry a line that long. The panel cannot, and the
	 * panel is the half she actually looks at.
	 *
	 * `PostSticky` keys a row by `Key`, so splitting costs three gauge rows that each rewrite in place
	 * rather than one that wraps. It does NOT cost screen history: these are gauges, not events.
	 *
	 * The three answer three different questions, which is why this split and not some other:
	 *   Reason  — how bad, and WHAT caused it
	 *   where   — WHICH THREAD the time went to
	 *   pso     — the PSO buckets, their rates, and whether they can fire at all
	 * `FPMOverlay.h:43` requires the screen and the log to agree, so both get the same three lines.
	 */
	FString Head = FString::Printf(
		TEXT("%s | %d hitch(es) in %llu frame(s) over %.1f s (threshold %.0f ms)"),
		Reason, Hitches, static_cast<unsigned long long>(FramesInWindow), WindowSeconds,
		CVarHitchThresholdMs.GetValueOnAnyThread());
	FString Where;
	FString Pso;

	if (Hitches > 0)
	{
		// BOTH halves, always, and never folded together. The async-flush count alone was structurally
		// blind to sync loads, so reporting it on its own is what made a partial zero look like a whole one.
		// Abbreviated from prose to a labelled list purely for width — the counters are unchanged.
		Head += FString::Printf(
			TEXT(" | worst %.1f ms, mean %.1f ms | cause: %d flush, %d sync, %d gc, %d cold-pso, "
			     "%d pso-work, %d pso-precompile"),
			WorstHitchMs, MeanMs, HitchesWithFlush, HitchesWithSyncLoad, HitchesWithGc, HitchesWithPsoCreate,
			HitchesWithPsoWork, HitchesWithPso);

		/*
		 * ★ THE UNATTRIBUTED SHARE IS THE NUMBER THIS WIDENING LIVES OR DIES BY, so it is printed as a
		 * PERCENTAGE and not left to be worked out from the four counters above.
		 *
		 * Design `:1218` asks for exactly this: "an unattributed-stall RATE, printed, so 'most stalls
		 * anonymous' becomes a number that can fall." Without it, adding a fifth bucket reads as progress
		 * whether or not the anonymous share actually moved -- and on 0.6.0 that share was 21 of 21.
		 * `Hitches > 0` is guaranteed inside this branch, so the division is safe.
		 */
		Head += FString::Printf(TEXT(" | %d UNATTRIBUTED (%.0f%% of hitches)"),
			HitchesUnattributed, 100.0 * HitchesUnattributed / Hitches);

		/*
		 * ★ WHERE THE TIME WENT, printed beside WHAT caused it, because the two answer different
		 * questions and the pairing is what makes an unattributed hitch actionable. "8 unattributed, all
		 * game-thread bound" and "8 unattributed, all neither-thread-busy" send you to opposite ends of
		 * the engine, and until now both printed as the same line.
		 */
		if (GtFramesSeen.load(std::memory_order_relaxed) > 0)
		{
			Where = FString::Printf(
				TEXT("where: %d game-thread bound, %d render-thread bound, %d neither (gpu/vsync/os) | "
				     "worst ON A HITCH: gt %.1f ms, rt %.1f ms | %lld GT / %lld RT frame(s) seen"),
				HitchesGameThreadBound, HitchesRenderThreadBound, HitchesNeitherThreadBusy,
				WorstGtBusyMs, WorstRtBusyMs,
				static_cast<long long>(GtFramesSeen.load(std::memory_order_relaxed)),
				static_cast<long long>(RtFramesSeen.load(std::memory_order_relaxed)));
		}
		else
		{
			// ⚠ The denominator discipline this file opens with, applied to the split. Saying nothing here
			// would let the reader assume the where-verdict simply had nothing to report.
			Where = FString::Printf(
				TEXT("where: UNAVAILABLE for all %d hitch(es) — no OnEndFrame broadcast has been seen, "
				     "so the game/render split is DEAD, not quiet"),
				HitchesSplitUnavailable);
		}
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
		Pso += FString::Printf(
			TEXT(" | PSO precompile overlapped %d span(s): %d hitch(es) there (%.2f%%) vs %d in the other "
			     "%llu (%.2f%%)"),
			FramesDuringPso, HitchesWithPso, 100.0 * HitchesWithPso / FramesDuringPso,
			HitchesOutside, static_cast<unsigned long long>(FramesOutside),
			FramesOutside > 0 ? 100.0 * HitchesOutside / static_cast<double>(FramesOutside) : 0.0);
	}

	/*
	 * ★ THE SAME RATE TREATMENT FOR ASYNC PSO WORK, because it is a level and levels flatter themselves.
	 * Unlike the precompile run this one CAN span normal play — `r.PSOPrecache` fires while streaming — so
	 * the in-flight versus out-of-flight comparison is the only form in which it means anything. Printed
	 * only when work actually overlapped this window.
	 */
	if (FramesDuringPsoWork > 0)
	{
		const uint64 FramesOutside = FramesInWindow > static_cast<uint64>(FramesDuringPsoWork)
			? FramesInWindow - static_cast<uint64>(FramesDuringPsoWork)
			: 0;
		const int32 HitchesOutside = Hitches - HitchesWithPsoWork;
		Pso += FString::Printf(
			TEXT(" | async PSO work overlapped %d span(s) (peak %d precompile task(s), %d precache "
			     "request(s)): %d hitch(es) there (%.2f%%) vs %d in the other %llu (%.2f%%)"),
			FramesDuringPsoWork, PeakPrecompileTasks, PeakPrecacheRequests, HitchesWithPsoWork,
			100.0 * HitchesWithPsoWork / FramesDuringPsoWork,
			HitchesOutside, static_cast<unsigned long long>(FramesOutside),
			FramesOutside > 0 ? 100.0 * HitchesOutside / static_cast<double>(FramesOutside) : 0.0);
	}
	if (LoadStalls > 0)
	{
		// The stalls carry their own flush count. Folding them into the hitch figure would overstate the
		// hitch rate; dropping the count entirely is what review finding B caught.
		Pso += FString::Printf(
			TEXT(" | %d load stall(s) over %.0f ms (%d flush, %d sync, %d gc, %d cold-pso, %d pso-work, "
			     "%d pso-precompile, %d UNATTRIBUTED), not counted as hitches"),
			LoadStalls, CVarHitchIgnoreAboveMs.GetValueOnAnyThread(), LoadStallsWithFlush, StallsWithSyncLoad,
			StallsWithGc, StallsWithPsoCreate, StallsWithPsoWork, StallsWithPso, StallsUnattributed);
	}
	Pso += FString::Printf(
		TEXT(" | session totals: %d flush(es), %d sync load(s), %d GC pass(es), %d cold PSO creation(s)"),
		FlushesTotal.GetValue(), SyncLoadsTotal.GetValue(), GcTotal.GetValue(), PsoCreatesTotal.GetValue());

	/*
	 * ★ THE LIVENESS PROOF, PRINTED EXACTLY WHERE A ZERO WOULD OTHERWISE BE UNREADABLE.
	 *
	 * If nothing PSO-shaped has been seen all session, the four PSO fields above are all 0 — and 0 has two
	 * meanings: "no PSO work happened" and "none of this could ever have reported anything". Three of the
	 * inputs are gated by console variables FPM does not own. So when the totals are empty, the capability
	 * is stated instead of left to be assumed. When they are non-zero the instrument has proved itself and
	 * this stays quiet.
	 *
	 * This is the `FPMCVarWriter` pattern: do not claim the path works, demonstrate it — and where the
	 * demonstration is a zero, show the gate.
	 */
	/*
	 * ⚠ EACH BUCKET'S CAPABILITY IS GATED ON ITS OWN EMPTINESS — review finding, 2026-08-10. The first
	 * version gated the WHOLE line, including `precaching`, on the cold-creation and precompile-run
	 * counters being zero. Cold creations will normally be non-zero (100 measured in eleven minutes of
	 * her 03:27 session), which suppressed the line entirely — and `precaching` gates a THIRD bucket
	 * (`FramesDuringPsoWork`) that would then sit at zero with its explanation hidden. `PSOPrecache` had
	 * zero matches across her client logs, so an unsupported precache path is a live possibility here,
	 * not a hypothetical.
	 */
	if (PsoCreatesTotal.GetValue() == 0)
	{
		Pso += FString::Printf(
			TEXT(" | cold-PSO bucket zero — capability: filecache=%d, reportPSO=%d"),
			FPipelineFileCacheManager::IsPipelineFileCacheEnabled() ? 1 : 0,
			FPipelineFileCacheManager::ReportNewPSOs() ? 1 : 0);
	}
	if (FramesDuringPsoWork == 0)
	{
		Pso += FString::Printf(TEXT(" | no async PSO work seen — capability: precaching=%d"),
			PipelineStateCache::IsPSOPrecachingEnabled() ? 1 : 0);
	}

	// Run-level context, appended only once a run has actually completed. The seconds figure is labelled as
	// compile time rather than stall time every place it is printed, because it is the former.
	if (const int32 PsoRuns = PsoRunsCompleted.load(std::memory_order_relaxed); PsoRuns > 0)
	{
		Pso += FString::Printf(
			TEXT(", %d PSO precompile run(s) (last: %u task(s), %.2f s compile time)"),
			PsoRuns, PsoTasksLastRun.load(std::memory_order_relaxed),
			PsoSecondsLastRun.load(std::memory_order_relaxed));
	}
	if (LinesSuppressed > 0)
	{
		// Stated, never silent. A capped log that does not say it capped reads as a quiet session.
		Pso += FString::Printf(TEXT(" | %d hitch line(s) SUPPRESSED by the per-window cap"), LinesSuppressed);
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
		// Three gauge rows rather than one that runs off both edges of the screen. Each keeps its own slot
		// and rewrites in place, so this costs three stable rows and no scroll history.
		FPMOverlay::PostSticky(TEXT("hitch meter"), Reason, Head);
		if (!Where.IsEmpty()) { FPMOverlay::PostSticky(TEXT("hitch meter"), TEXT("where"), Where); }
		if (!Pso.IsEmpty())   { FPMOverlay::PostSticky(TEXT("hitch meter"), TEXT("detail"), Pso); }
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
	// ⚠ `PsoCreatesInFrame` is NOT reset here either, and for a different reason from `bPsoRunActive`:
	// `ClassifySpan` consumes it every span, so it is already ~0 by now, and clearing it on a window
	// boundary could discard a creation that landed between the last span and this line. The session
	// totals (`PsoCreatesTotal` and the three type splits) are session-scoped and never reset.
	HitchesWithPsoCreate = 0;
	StallsWithPsoCreate = 0;
	FramesDuringPsoWork = 0;
	HitchesWithPsoWork = 0;
	StallsWithPsoWork = 0;
	PeakPrecompileTasks = 0;
	PeakPrecacheRequests = 0;
	HitchesUnattributed = 0;
	StallsUnattributed = 0;
	// ⚠ The work-or-wait ACCUMULATORS are not reset here — `ClassifySpan` exchanges them every span, and
	// clearing them on a window boundary could discard a frame that has already been measured but whose
	// span has not closed. Only the per-window verdict tallies reset.
	HitchesGameThreadBound = 0;
	HitchesRenderThreadBound = 0;
	HitchesNeitherThreadBusy = 0;
	HitchesSplitUnavailable = 0;
	WorstGtBusyMs = 0.0;
	WorstRtBusyMs = 0.0;
	LinesThisWindow = 0;
	LinesSuppressed = 0;
	WorstHitchMs = 0.0;
	HitchMsTotal = 0.0;
}

void FFPMHitchMeter::LogPsoReport()
{
	/*
	 * ★ CAPABILITY FIRST, COUNTS SECOND. Every number below can legitimately be zero, and a zero is only
	 * readable next to the gate that produces it. All four reads are null-safe on a dedicated server,
	 * checked rather than assumed because this fix is `Side() == Any` and a null-deref here would take
	 * down the server rather than misreport:
	 *   - `IsPipelineFileCacheEnabled()` / `ReportNewPSOs()` read a static bool and a cvar.
	 *   - `IsPSOPrecachingEnabled()` is `GPSOPrecaching != 0 && GRHISupportsPSOPrecaching`
	 *     (`PipelineStateCache.cpp:4127-4135`); NullRHI never sets the latter.
	 *   - `NumActivePrecacheRequests()` returns 0 on that same check BEFORE touching its globals
	 *     (`:4339-4344`), and `FShaderPipelineCache::IsPrecompiling()` null-checks its singleton
	 *     (`ShaderPipelineCache.cpp:1727-1734`).
	 */
	const bool bFileCache  = FPipelineFileCacheManager::IsPipelineFileCacheEnabled();
	const bool bReportPSOs = FPipelineFileCacheManager::ReportNewPSOs();
	const bool bPrecaching = PipelineStateCache::IsPSOPrecachingEnabled();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] PSO capability: file cache %s, new-PSO reporting %s, PSO precaching %s. "
		     "The cold-creation count below can only move when new-PSO reporting is ON."),
		bFileCache  ? TEXT("ON")  : TEXT("OFF"),
		bReportPSOs ? TEXT("ON")  : TEXT("OFF"),
		bPrecaching ? TEXT("ON")  : TEXT("OFF"));

	// ⚠ Stated plainly rather than left for the reader to infer from three ON/OFF words. A dead bucket
	// that does not say it is dead is the exact failure this meter was built against.
	UE_CLOG(!bReportPSOs, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] r.ShaderPipelineCache.ReportPSO is OFF, so the cold-PSO bucket CANNOT report a "
		     "non-zero. Read its 0 as 'not measured', not as 'no cold pipelines were built'."));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] cold PSO creations this session: %d total (%d graphics, %d compute, %d ray tracing). "
		     "Each one is a pipeline that was not in the cache and had to be built during play."),
		PsoCreatesTotal.GetValue(), PsoCreatesGraphics.GetValue(), PsoCreatesCompute.GetValue(),
		PsoCreatesRayTracing.GetValue());

	// Cross-check against the engine's own tally. It counts only new PSOs with at least one bind or a
	// compile failure, and it is gated on LogPSO as well as Enabled, so it can disagree with ours -- a
	// disagreement is information, which is why both are printed instead of just the one we control.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] engine cross-check: FPipelineFileCacheManager::NumPSOsLogged() = %u (gated on file "
		     "cache AND r.ShaderPipelineCache.LogPSO; 0 here with a non-zero count above means logging is "
		     "off, not that nothing was built)."),
		FPipelineFileCacheManager::NumPSOsLogged());

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] PSO work right now: %d precompile task(s) in flight, %d precache request(s) active, "
		     "bundled precompile %s. Precompile runs completed this session: %d (last: %u task(s), "
		     "%.2f s of compile time summed across the run)."),
		PipelineStateCache::GetNumActivePipelinePrecompileTasks(),
		PipelineStateCache::NumActivePrecacheRequests(),
		FShaderPipelineCache::IsPrecompiling() ? TEXT("RUNNING") : TEXT("idle"),
		PsoRunsCompleted.load(std::memory_order_relaxed),
		PsoTasksLastRun.load(std::memory_order_relaxed),
		PsoSecondsLastRun.load(std::memory_order_relaxed));
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

/*
 * `FPM.Pso.Report` — the PSO picture on its own, capability first.
 *
 * Separate from `FPM.Hitch.Report` because it answers a different question. The hitch summary asks "what
 * happened in this window"; this asks "is the PSO measurement alive, and what has it seen all session".
 * The second question is the one that has to be settled before any PSO number is worth reading.
 */
static FAutoConsoleCommand GPsoReportCmd(
	TEXT("FPM.Pso.Report"),
	TEXT("Print the PSO picture: whether each bucket can fire, cold creations this session, and current "
	     "precompile/precache work in flight."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMHitchMeter::Get().LogPsoReport();
	}));
