// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMHitchMeter.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "HAL/IConsoleManager.h"
#include "ContentStreaming.h"
// UWorld::HasAnyLevelMakingVisible / HasAnyLevelMakingInvisible, the game-thread half of level streaming.
#include "Engine/World.h"
// GLog->AddOutputDevice / RemoveOutputDevice, for the log-volume sink.
#include "Misc/OutputDeviceRedirector.h"
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
 * THE LOG-BURST BAR IS A CVAR BECAUSE IT IS THE ONE NUMBER IN THIS WIDENING THAT COULD MANUFACTURE A
 * FALSE ATTRIBUTION, so it has to be arguable rather than buried.
 *
 * Set it too low and every frame is a "log burst" and the bucket claims the whole window. The default is
 * chosen to sit far outside normal traffic rather than on its edge: her 2026-08-15 client log runs in
 * single-digit lines per frame during play, and the burst that preceded the 409 ms hitch was 17,465 in
 * ONE frame. Anything between those two numbers separates them, so 200 is a wide margin on both sides
 * and not a tuned constant.
 */
static TAutoConsoleVariable<int32> CVarHitchLogBurstLines(
	TEXT("FPM.Hitch.LogBurstLines"), 200,
	TEXT("Log lines written inside one span, at or above which the span is attributed to a LOG BURST. The "
	     "count is always printed; only the attribution uses this bar. 0 disables the bucket, which then "
	     "reports its own count with no claim attached. Default 200."),
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
 * ★ HOW LONG AFTER A WORLD LOAD STILL COUNTS AS STARTUP.
 *
 * Thirty seconds, and it is a judgement rather than a measurement — chosen to be generously long, since
 * the failure that matters here is counting the arrival burst as mid-play and arguing for an ini
 * exception on inflated numbers. Erring long can only UNDERSTATE the case for pre-optimize, which is the
 * safe direction when the thing being decided is permanent residue on a player's machine.
 */
constexpr double GFPMPsoSettleSeconds = 30.0;

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
/**
 * Returns whether the `LOG` command was actually handled (`FSelfRegisteringExec::StaticExec`'s own
 * return value), not just whether it was sent. ⚠ Discarding that return used to be the shape of this
 * function: the caller then logged a flat "raised to Verbose" regardless of whether any registered exec
 * consumed the command. Fixed 2026-08-15 - callers now check this and report honestly.
 */
static bool FPMSetEnginePsoHitchLogging(bool bVerbose)
{
	if (GLog == nullptr) { return false; }

	// `Log` is LogPSOHitching's own declared default (`PipelineStateCache.cpp:45`), so this restores
	// rather than guesses. `Log Reset` would have reset EVERY category, which is not ours to do.
	const TCHAR* Cmd = bVerbose
		? TEXT("Log LogPSOHitching Verbose")
		: TEXT("Log LogPSOHitching Log");

	return FSelfRegisteringExec::StaticExec(nullptr, Cmd, *GLog);
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

	// The log-volume bucket. Registered here and removed in Disarm(), so the ZERO RESIDUE rule holds:
	// nothing of FPM stays attached to GLog after uninstall. Guarded because GLog can be null during very
	// early startup and during shutdown, and a null deref inside the instrument would take the game with
	// it for the sake of a counter.
	if (GLog)
	{
		GLog->AddOutputDevice(&LogSink);
	}

	// Reveal the engine's own per-creation timing line. See the long note above the cvar.
	// ⚠ Never on a dedicated server: NullRHI builds no pipelines, so this would raise a category that
	// cannot emit and put a misleading switch in the server log for nothing.
	if (!IsRunningDedicatedServer() && CVarPsoEngineHitchLog.GetValueOnAnyThread() != 0)
	{
		// ⚠ THE RETURN VALUE IS CHECKED, NOT ASSUMED. Until 2026-08-15 this logged the same "raised to
		// Verbose" line whether or not `StaticExec` actually handled the `LOG` command - a build where
		// nothing consumed it would still claim success, and the honest signal (no per-pipeline hitch
		// lines ever appear) would be silent.
		if (FPMSetEnginePsoHitchLogging(true))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] engine PSO-hitch logging raised to Verbose. The game will now print one "
				     "'Runtime graphics/compute PSO creation hitch (N msec)' line per pipeline built above "
				     "r.PSO.RuntimeCreationHitchThreshold. Set FPM.Pso.EngineHitchLog 0 to leave it alone."));
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] could NOT raise engine PSO-hitch logging to Verbose: the 'LOG LogPSOHitching "
				     "Verbose' command was not handled (no registered exec consumed it, or GLog was "
				     "unavailable). Per-pipeline hitch lines will NOT appear this session. Set "
				     "FPM.Pso.EngineHitchLog 0 to silence this warning if that is expected."));
		}
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

	// Removed before the delegates, and before anything else here can log. `RemoveOutputDevice` on a
	// device that was never added is a no-op array removal, so the never-armed path is safe too.
	if (GLog)
	{
		GLog->RemoveOutputDevice(&LogSink);
	}

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

	/*
	 * ★ THE ONE NUMBER THAT DECIDES THE PRE-OPTIMIZE QUESTION, and the reason it is a separate counter.
	 *
	 * Ant, 2026-08-10, on whether FPM2 should spend an ini exception on
	 * `r.ShaderPipelineCache.PreOptimizeEnabled`: *"Defer until we have a startup measurement."*
	 *
	 * Verified from the retail cooked config this session — `FactoryGame/Config/DefaultEngine.ini:15`
	 * sets `r.ShaderPipelineCache.Enabled=1` and nothing else, so `PreOptimizeEnabled` sits at the
	 * engine's own default of 0 (`ShaderPipelineCache.cpp:138-142`). Pre-optimize IS off in her game, so
	 * there is something to turn on. What is unknown is whether turning it on would buy anything.
	 *
	 * Pre-optimize front-loads PSO compilation into startup. It can only help PSOs that would otherwise
	 * be compiled LATER — during play. So the size of the prize is exactly this: how many PSOs get
	 * created once the world has settled. `PsoCreatesTotal` cannot answer it, because it is dominated by
	 * the startup burst that pre-optimize would merely move rather than remove.
	 *
	 * ⚠ IT IS NOT GATED ON A HITCH, on purpose. A 4 ms PSO compile costs real time and never trips the
	 * hitch threshold, so counting only the ones that hitched would undercount the prize and could
	 * report a confident zero on a session that spent seconds compiling.
	 *
	 * READ IT AS: near zero over a long session means the shipped cache already covers her play and
	 * pre-optimize would front-load work she never needed — the exception is dead. A large number means
	 * that cost is being paid mid-game and moving it to startup is worth the residue argument.
	 */
	const double Settled = SettledRealSeconds.load(std::memory_order_relaxed);
	if (Settled > 0.0 && FPlatformTime::Seconds() >= Settled)
	{
		PsoCreatesAfterSettle.Increment();
	}

	// No log line per PSO, deliberately. Her 03:27 session created enough of these to reach the engine's
	// own 100-hitch marker, and one line each would bury every other channel. The per-span count in the
	// hitch line and the session split in FPM.Pso.Report are what the question actually needs.
}

void FFPMHitchMeter::OnPsoPrecompileBegin(uint32 Count)
{
	bPsoRunActive.store(true, std::memory_order_relaxed);

	// ⚠ RENDER THREAD (`FShaderPipelineCache : FTickableObjectRenderThread`, `ShaderPipelineCache.h:78`).
	// UE_LOG is thread-safe, and `FPMDiag::IsOn` is already called off the game thread by
	// `OnAsyncLoadPackage` in this same file. `FPMOverlay` IS ALSO thread-safe: it takes `FScopeLock`
	// inside `Post()` itself (FPMOverlay.cpp:170), one of five lock sites in that file, so a post from
	// this render-thread callback would not race a game-thread post. Nothing here posts to the screen
	// anyway, by choice rather than necessity: the overlay carries the recurring PSO summary line, not
	// one-off begin/complete events, and the summary posts from the game thread, where it belongs.
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

	/*
	 * ★ WHEN "STARTUP" ENDS, for the pre-optimize measurement in `OnPsoCreated`.
	 *
	 * A world load is the only honest boundary available here. The loading screen is still up when this
	 * fires, so a grace period follows it — PSOs compiled in the first seconds of a world are still the
	 * arrival burst, and counting them as mid-play would inflate the prize and argue for an ini exception
	 * the numbers do not support.
	 *
	 * ⚠ IT IS RESET ON EVERY LOAD, INCLUDING AUTOSAVE-DRIVEN ONES. That is deliberate: each world gets
	 * its own settle window, so a quit-to-menu-and-back does not leave the counter treating the next
	 * world's startup burst as mid-play.
	 */
	SettledRealSeconds.store(FPlatformTime::Seconds() + GFPMPsoSettleSeconds, std::memory_order_relaxed);

	/*
	 * THE ONLY WORLD REFERENCE THIS METER KEEPS, and it is weak on purpose. `ClassifySpan` runs from the
	 * core ticker, which outlives every world: a raw pointer here would be read once per frame against a
	 * world that quit to the menu three seconds ago. `TWeakObjectPtr` turns that from a crash into a
	 * `nullptr`, and the summary states which of the two it got rather than printing a confident zero.
	 */
	MeteredWorld = World;
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
	 * BUCKET 8, LOG VOLUME, and it is CONSUMED HERE rather than at the call sites for a reason that is
	 * about correctness and not about tidiness: this function LOGS. Its HITCH line, and the three summary
	 * rows that follow it, all pass through the sink. Consuming the counter at the top means this meter's
	 * own output is counted into the NEXT span, where about four lines cannot cross a 200-line bar. Read
	 * it any later and the instrument would start attributing hitches to itself.
	 */
	const int32 LogLinesInSpan = LogSink.LinesInFrame.Set(0);
	LogLinesInWindow += LogLinesInSpan;
	WorstLogLinesInSpan = FMath::Max(WorstLogLinesInSpan, LogLinesInSpan);
	const int32 LogBurstBar = CVarHitchLogBurstLines.GetValueOnAnyThread();
	const bool bLogBurstInSpan = LogBurstBar > 0 && LogLinesInSpan >= LogBurstBar;

	/*
	 * BUCKET 9, THE GC TAIL. Two free functions, no editor guard, no allocation, no lock: each reads a
	 * global the collector maintains (`UObjectGlobals.h:937`, `:944`).
	 *
	 * The existing `gc` bucket counts the PreGarbageCollect broadcast, which is the START of a
	 * collection. Unhashing and purging run on LATER frames, as game-thread work, and until now fell into
	 * no bucket at all. `bGcPurgeEverSeen` is session-scoped and never reset, so a window holding a zero
	 * can distinguish "the tail was quiet this minute" from "this pair has never once been true".
	 */
	const bool bGcPurgeInSpan = IsIncrementalPurgePending() || IsIncrementalUnhashPending();
	if (bGcPurgeInSpan)
	{
		++FramesDuringGcPurge;
		bGcPurgeEverSeen = true;
	}

	/*
	 * BUCKET 10, LEVEL VISIBILITY. The game-thread half of streaming: AddToWorld and RemoveFromWorld
	 * register and unregister a streamed level's components incrementally, on the game thread, which is
	 * where every one of her measured hitches was bound.
	 *
	 * The 5.6 predicates, NOT the 5.5 accessors. `GetCurrentLevelPendingVisibility` and
	 * `GetCurrentLevelPendingInvisibility` still compile (`World.h:1038`, `:1048`) and return literal
	 * `nullptr`, so a bucket built on them would be born dead and confident.
	 */
	bool bLevelVisInSpan = false;
	if (const UWorld* World = MeteredWorld.Get())
	{
		bLevelVisWorldSeen = true;
		bLevelVisInSpan = World->HasAnyLevelMakingVisible() || World->HasAnyLevelMakingInvisible();
		if (bLevelVisInSpan)
		{
			++FramesDuringLevelVis;
			bLevelVisEverSeen = true;
		}
	}

	/*
	 * ★ THE STREAMER'S BACKLOG — the seventh bucket, sampled HERE because this runs once per span and
	 * the value is a LEVEL rather than an event. See the long note in the header for why it exists.
	 *
	 * Two plain int32 getters on a process-global collection. `GetNumWantingResources()` is the backlog
	 * depth; `GetNumWantingResourcesID()` moves only when the streaming system actually updates, which is
	 * what makes a zero backlog distinguishable from a readout nothing is feeding.
	 */
	int32 StreamingInSpan = 0;
	{
		FStreamingManagerCollection& Streaming = IStreamingManager::Get();
		const int32 Wanting   = Streaming.GetNumWantingResources();
		const int32 WantingId = Streaming.GetNumWantingResourcesID();

		if (LastStreamingWantingId != -1 && WantingId != LastStreamingWantingId)
		{
			bStreamingIdEverMoved = true;
		}
		LastStreamingWantingId = WantingId;

		StreamingWantingPeakInSpan = FMath::Max(StreamingWantingPeakInSpan, Wanting);
		StreamingWantingWorst      = FMath::Max(StreamingWantingWorst, Wanting);
		if (Wanting > 0) { ++FramesDuringStreaming; }

		// Consumed: the peak belongs to the span that just closed.
		StreamingInSpan = StreamingWantingPeakInSpan;
		StreamingWantingPeakInSpan = 0;
	}

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
	//
	// The three added on 2026-08-15 sit at the end in ascending order of doubt, matching the ordering rule
	// already used above. The log burst is an EVENT and the strongest of the three. The GC tail and the
	// level-visibility flag are LEVELS, so each carries its own frame denominator into the summary, and
	// neither is allowed to lower the unattributed count without printing the rate that justifies it.
	const bool bAttributed = FlushesInSpan > 0 || SyncLoadsInSpan > 0 || GcInSpan > 0 || bPsoInSpan
		|| StreamingInSpan > 0
		|| PsoCreatesInSpan > 0 || bPsoWorkInSpan
		|| bLogBurstInSpan || bGcPurgeInSpan || bLevelVisInSpan;

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
		if (StreamingInSpan > 0)  { ++StallsWithStreaming; }
		if (bLogBurstInSpan)      { ++StallsWithLogBurst; }
		if (bGcPurgeInSpan)       { ++StallsWithGcPurge; }
		if (bLevelVisInSpan)      { ++StallsWithLevelVis; }
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
	if (StreamingInSpan > 0)  { ++HitchesWithStreaming; }
	if (bLogBurstInSpan)      { ++HitchesWithLogBurst; }
	if (bGcPurgeInSpan)       { ++HitchesWithGcPurge; }
	if (bLevelVisInSpan)      { ++HitchesWithLevelVis; }
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

	/*
	 * The three fields added on 2026-08-15, each worded at the strength of its evidence.
	 *
	 * The log count prints whenever it crosses the bar, as a COUNT and not a verdict, because a burst can
	 * be the consequence of a stall as easily as its cause. The other two say "during", the same wording
	 * the PSO run gets, because both are levels that can span a whole window.
	 */
	FString LogPart;
	if (bLogBurstInSpan)
	{
		LogPart = FString::Printf(TEXT(" | LOG BURST: %d line(s) written in this span"), LogLinesInSpan);
	}
	const TCHAR* GcPurgePart  = bGcPurgeInSpan  ? TEXT(" | during a GC unhash/purge tail") : TEXT("");
	const TCHAR* LevelVisPart = bLevelVisInSpan
		? TEXT(" | during level visibility work (AddToWorld/RemoveFromWorld)") : TEXT("");

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
		     "package(s) still loading%s%s%s%s%s%s%s%s%s"),
		SpanMs, bClosedByLoad ? TEXT(" (closed by a world load)") : TEXT(""), *ThreadPart,
		FlushesInSpan, GcInSpan, GetNumAsyncPackages(), *SyncPart, *PsoCreatePart, *PsoWorkPart, PsoPart,
		*LogPart, GcPurgePart, LevelVisPart,
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
		// The fifth in-frame counter, discarded for the same stated reason as the other four: a loading
		// screen writes a great many log lines, and folding them into the first playable span would
		// manufacture a LOG BURST attribution on the one span most likely to hitch anyway.
		LogSink.LinesInFrame.Set(0);

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
		/*
		 * THE UNATTRIBUTED COUNT IS THE LAST ITEM OF THE CAUSE LIST, NOT A SEPARATE CLAUSE BEHIND IT, and
		 * that placement is the whole fix. It was already printed, further along this same string, behind
		 * a 130-character streaming-liveness bracket. On 2026-08-15 Ant read the line, saw
		 * `0 flush, 1 sync, 0 gc, 0 cold-pso, 0 pso-work, 0 pso-precompile, 0 streaming` against four
		 * hitches, and reported that three of the four were unreported. She was reading a list that
		 * LOOKED complete because the number completing it was off the end of the row.
		 *
		 * Inside the list it is unmissable, and the list now adds up in front of the reader. A count in
		 * the log that nobody can find is worth about what a count that was never taken is worth.
		 */
		Head += FString::Printf(
			TEXT(" | worst %.1f ms, mean %.1f ms | cause: %d flush, %d sync, %d gc, %d gc-purge, "
			     "%d cold-pso, %d pso-work, %d pso-precompile, %d streaming, %d level-vis, %d log-burst, "
			     "%d UNATTRIBUTED (%.0f%%)"),
			WorstHitchMs, MeanMs, HitchesWithFlush, HitchesWithSyncLoad, HitchesWithGc, HitchesWithGcPurge,
			HitchesWithPsoCreate, HitchesWithPsoWork, HitchesWithPso, HitchesWithStreaming,
			HitchesWithLevelVis, HitchesWithLogBurst,
			HitchesUnattributed, 100.0 * HitchesUnattributed / Hitches);

		/*
		 * EVERY LIVENESS BRACKET NOW LIVES ON THE `detail` ROW, and moving them there is what paid for the
		 * three new fields above without making this row wider than it already was.
		 *
		 * The streaming bracket alone was 130 characters sitting between the cause list and the number the
		 * cause list needs. This row is the one Ant reads on screen, and the note above the three-row split
		 * says why width here is a real cost rather than a style preference. A capability statement is
		 * context for a zero, not the reading itself, so it belongs beside the other capability statements.
		 *
		 * The unattributed share still carries its PERCENTAGE, and design `:1218` still asks for exactly
		 * that: "an unattributed-stall RATE, printed, so 'most stalls anonymous' becomes a number that can
		 * fall." It has moved INTO the cause list above, not away.
		 */

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
	/*
	 * THE STREAMING BUCKET'S OWN LIVENESS LINE, printed beside its count and never without it. Moved off
	 * the headline row on 2026-08-15 for width; the reasoning below is unchanged.
	 *
	 * `GetNumWantingResourcesID()` moves only when the streaming system updates. If it has never moved, a
	 * zero backlog means "nothing is feeding this readout", which is a completely different statement from
	 * "the streamer was never behind", and it is the statement that would otherwise be silently mistaken
	 * for the good news. Gated on `FramesInWindow` so the empty shutdown summary stays quiet.
	 */
	if (FramesInWindow > 0)
	{
		Pso += bStreamingIdEverMoved
			? FString::Printf(
				TEXT(" | streaming live, worst backlog %d resource(s), behind on %d frame(s)"),
				StreamingWantingWorst, FramesDuringStreaming)
			: FString::Printf(
				TEXT(" | streaming bucket UNPROVEN: GetNumWantingResourcesID has never moved, so its %d "
				     "is 'not measured', not 'never behind'"), HitchesWithStreaming);
	}

	/*
	 * THE GC TAIL AS A RATE, for the reason the two PSO levels already document: a level flatters itself.
	 * "3 hitches during a purge tail" means nothing if the tail covered the window, and it is a finding if
	 * it covered forty frames. The in-versus-out comparison is the only form in which a level bucket is
	 * allowed to lower the unattributed count on the row above.
	 */
	if (FramesDuringGcPurge > 0)
	{
		const uint64 FramesOutside = FramesInWindow > static_cast<uint64>(FramesDuringGcPurge)
			? FramesInWindow - static_cast<uint64>(FramesDuringGcPurge)
			: 0;
		const int32 HitchesOutside = Hitches - HitchesWithGcPurge;
		Pso += FString::Printf(
			TEXT(" | GC unhash/purge tail pending on %d frame(s): %d hitch(es) there (%.2f%%) vs %d in "
			     "the other %llu (%.2f%%)"),
			FramesDuringGcPurge, HitchesWithGcPurge, 100.0 * HitchesWithGcPurge / FramesDuringGcPurge,
			HitchesOutside, static_cast<unsigned long long>(FramesOutside),
			FramesOutside > 0 ? 100.0 * HitchesOutside / static_cast<double>(FramesOutside) : 0.0);
	}
	else if (!bGcPurgeEverSeen && FramesInWindow > 0)
	{
		// The session-scoped flag is what makes this honest. A quiet minute and a bucket that has never
		// once been true print the same 0, and only one of them is news about the game.
		Pso += FString::Printf(
			TEXT(" | gc-purge bucket has NEVER been true this session: IsIncrementalPurgePending and "
			     "IsIncrementalUnhashPending both read false on every frame so far, so its %d is 'the tail "
			     "was never caught running', not 'the tail is cheap'"), HitchesWithGcPurge);
	}

	/*
	 * LEVEL VISIBILITY, same treatment and the same reason, plus a THIRD state the GC tail does not need:
	 * this bucket depends on a world reaching the meter through OnWorldLoad. "No world" and "a world that
	 * never streamed" print the same zero and mean opposite things.
	 */
	if (FramesDuringLevelVis > 0)
	{
		const uint64 FramesOutside = FramesInWindow > static_cast<uint64>(FramesDuringLevelVis)
			? FramesInWindow - static_cast<uint64>(FramesDuringLevelVis)
			: 0;
		const int32 HitchesOutside = Hitches - HitchesWithLevelVis;
		Pso += FString::Printf(
			TEXT(" | level visibility work on %d frame(s): %d hitch(es) there (%.2f%%) vs %d in the other "
			     "%llu (%.2f%%)"),
			FramesDuringLevelVis, HitchesWithLevelVis, 100.0 * HitchesWithLevelVis / FramesDuringLevelVis,
			HitchesOutside, static_cast<unsigned long long>(FramesOutside),
			FramesOutside > 0 ? 100.0 * HitchesOutside / static_cast<double>(FramesOutside) : 0.0);
	}
	else if (FramesInWindow > 0)
	{
		Pso += bLevelVisWorldSeen
			? FString(TEXT(" | level-vis bucket has never been true this session: a world is reachable and "
			               "HasAnyLevelMakingVisible/Invisible read false on every frame so far"))
			: FString(TEXT(" | level-vis bucket is BLIND: no world has reached this meter through "
			               "OnWorldLoad, so its 0 is 'not measured', not 'nothing streamed'"));
	}

	/*
	 * LOG VOLUME, and unlike the two above it prints EVERY window rather than only when it fires, because
	 * the count itself is the diagnostic. The bar is printed with it so a zero reads as "below the bar"
	 * rather than as "nothing logged", and the session total is the bucket's liveness proof: zero there
	 * means nothing ever reached the sink, which is a fault in this meter and not a quiet session.
	 */
	if (FramesInWindow > 0)
	{
		if (LogSink.LinesTotal.GetValue() == 0)
		{
			Pso += TEXT(" | log-volume bucket DEAD: not one line has reached the sink this session, so it "
			            "is not registered on GLog rather than the log being quiet");
		}
		else
		{
			Pso += FString::Printf(
				TEXT(" | log volume: %lld line(s) this window, worst %d in one span, bar %d, %d session "
				     "total"),
				static_cast<long long>(LogLinesInWindow), WorstLogLinesInSpan,
				CVarHitchLogBurstLines.GetValueOnAnyThread(), LogSink.LinesTotal.GetValue());
		}
	}

	if (LoadStalls > 0)
	{
		// The stalls carry their own flush count. Folding them into the hitch figure would overstate the
		// hitch rate; dropping the count entirely is what review finding B caught.
		Pso += FString::Printf(
			TEXT(" | %d load stall(s) over %.0f ms (%d flush, %d sync, %d gc, %d gc-purge, %d cold-pso, "
			     "%d pso-work, %d pso-precompile, %d level-vis, %d log-burst, %d UNATTRIBUTED), not "
			     "counted as hitches"),
			LoadStalls, CVarHitchIgnoreAboveMs.GetValueOnAnyThread(), LoadStallsWithFlush, StallsWithSyncLoad,
			StallsWithGc, StallsWithGcPurge, StallsWithPsoCreate, StallsWithPsoWork, StallsWithPso,
			StallsWithLevelVis, StallsWithLogBurst, StallsUnattributed);
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
	// The three buckets added 2026-08-15. Their per-window tallies reset here; the three SESSION-scoped
	// facts do NOT, and that is deliberate. `bGcPurgeEverSeen`, `bLevelVisEverSeen` and
	// `bLevelVisWorldSeen` answer "could this bucket ever report anything", which is a question about the
	// whole session, and clearing them every minute would make every window claim the bucket is dead.
	// `LogSink.LinesTotal` is session-scoped for the same reason. `LogSink.LinesInFrame` is not touched
	// here either: `ClassifySpan` consumes it every span, so it is already near zero, and clearing it on a
	// window boundary could discard lines written between the last span and this line.
	HitchesWithLogBurst = 0;
	StallsWithLogBurst = 0;
	WorstLogLinesInSpan = 0;
	LogLinesInWindow = 0;
	FramesDuringGcPurge = 0;
	HitchesWithGcPurge = 0;
	StallsWithGcPurge = 0;
	FramesDuringLevelVis = 0;
	HitchesWithLevelVis = 0;
	StallsWithLevelVis = 0;
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

	/*
	 * ★ THE PRE-OPTIMIZE VERDICT, and it names the decision it is feeding so the number is not orphaned.
	 *
	 * Ant deferred the `r.ShaderPipelineCache.PreOptimizeEnabled` ini exception on 2026-08-10 pending
	 * this measurement. Confirmed from the retail cooked config the same day:
	 * `FactoryGame/Config/DefaultEngine.ini:15` sets only `r.ShaderPipelineCache.Enabled=1`, so
	 * pre-optimize is at the engine default of 0 and IS available to turn on.
	 */
	const int32 AfterSettle = PsoCreatesAfterSettle.GetValue();

	const double SettledAt = SettledRealSeconds.load(std::memory_order_relaxed);

	if (SettledAt <= 0.0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   pre-optimize verdict: NO WORLD LOADED YET, so the mid-play window has not opened "
			     "and this measurement has not started. Not a result."));
	}
	else if (FPlatformTime::Seconds() < SettledAt)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   pre-optimize verdict: still inside the %.0f s settle window after the last world "
			     "load, so every PSO so far is the arrival burst. Play a while and run this again."),
			GFPMPsoSettleSeconds);
	}
	else if (!bReportPSOs)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   pre-optimize verdict: UNMEASURABLE. New-PSO reporting is off, so the mid-play "
			     "count cannot move and its %d is 'not measured'."), AfterSettle);
	}
	else if (AfterSettle == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   pre-optimize verdict: ZERO cold PSOs built more than %.0f s after a world load. "
			     "r.ShaderPipelineCache.PreOptimizeEnabled would front-load work that is never paid during "
			     "play, so it buys nothing here and does not justify an ini write. A real negative."),
			GFPMPsoSettleSeconds);
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   pre-optimize verdict: %d of %d cold PSO(s) were built MID-PLAY, more than %.0f s "
			     "after a world load. That is the work r.ShaderPipelineCache.PreOptimizeEnabled would move "
			     "to startup, and the size of the prize if we spend an ini exception on it."),
			AfterSettle, PsoCreatesTotal.GetValue(), GFPMPsoSettleSeconds);
	}

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
static FAutoConsoleCommandWithOutputDevice GHitchPackagesCmd(
	TEXT("FPM.Hitch.Packages"),
	TEXT("Print the synchronously-loaded packages this session, most frequent first."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMHitchMeter::Get().LogSyncPackages();
	}));

static FAutoConsoleCommandWithOutputDevice GHitchReportCmd(
	TEXT("FPM.Hitch.Report"),
	TEXT("Print the FPM hitch meter's running totals now, plus the session totals."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMHitchMeter::Get().LogSummary(TEXT("on request"));
	}));

/*
 * `FPM.Pso.Report` — the PSO picture on its own, capability first.
 *
 * Separate from `FPM.Hitch.Report` because it answers a different question. The hitch summary asks "what
 * happened in this window"; this asks "is the PSO measurement alive, and what has it seen all session".
 * The second question is the one that has to be settled before any PSO number is worth reading.
 */
/*
 * ⚠ WithOutputDevice, NOT the plain form — this printed NOTHING in the console until 2026-08-10.
 * See FPMConsoleEcho.h: a Display-level UE_LOG does not reach the console, so the whole report was
 * landing in FactoryGame.log while Ant watched a blank line and reasonably concluded it was broken.
 */
static FAutoConsoleCommandWithOutputDevice GPsoReportCmd(
	TEXT("FPM.Pso.Report"),
	TEXT("Print the PSO picture: whether each bucket can fire, cold creations this session, and current "
	     "precompile/precache work in flight."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMHitchMeter::Get().LogPsoReport();
	}));
