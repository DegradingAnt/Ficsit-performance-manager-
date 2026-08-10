// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMStallSampler.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/RunnableThread.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

/*
 * The threshold is deliberately BELOW the hitch meter's 50 ms. A sample taken at 50 ms would frequently
 * catch the tail of a frame that was about to end anyway; at 120 ms the frame is unambiguously stuck and
 * whatever it is inside has been there a while. Ant's measured stalls are 150-414 ms, so this sits well
 * under them and well over a normal 4 ms frame.
 */
static TAutoConsoleVariable<float> CVarStallSampleMs(
	TEXT("FPM.Stall.SampleAfterMs"), 120.0f,
	TEXT("Sample the game thread's callstack once a frame has been running this long. Must sit above "
	     "your normal frame time and below the stalls you are hunting. Default 120."),
	ECVF_Default);

/*
 * ⚠ A HARD SESSION CAP, because this instrument SUSPENDS THE GAME THREAD to read it.
 *
 * A few hundred microsecond suspends across a session is nothing. Ten thousand is a performance mod
 * causing hitches, which would be the joke writing itself. The cap is a number rather than a hope, and
 * the report prints how many of it were used so a truncated sample set cannot read as a complete one.
 */
static TAutoConsoleVariable<int32> CVarStallSampleBudget(
	TEXT("FPM.Stall.SessionBudget"), 200,
	TEXT("Maximum game-thread stack samples per session. Each one briefly suspends the game thread, so "
	     "this is a hard ceiling rather than a suggestion. 0 disables sampling entirely. Default 200."),
	ECVF_Default);

/** Minimum gap between samples. Stops one long stall producing a burst of near-identical captures. */
static TAutoConsoleVariable<float> CVarStallSampleGapMs(
	TEXT("FPM.Stall.MinGapMs"), 250.0f,
	TEXT("Minimum milliseconds between two stack samples. Default 250."),
	ECVF_Default);

/** How often the watchdog wakes to check the heartbeat. Cheap: one atomic load and a compare. */
static constexpr float WatchdogTickMs = 15.0f;

/** Deep enough to cross the engine's own dispatch layers and reach the mod frame that matters. */
static constexpr uint32 MaxStackDepth = 96;

FFPMStallSampler& FFPMStallSampler::Get()
{
	static FFPMStallSampler Instance;
	return Instance;
}

void FFPMStallSampler::SnapshotModules()
{
	Modules.Reset();

	const int32 Count = FPlatformStackWalk::GetProcessModuleCount();
	if (Count <= 0) { return; }

	TArray<FStackWalkModuleInfo> Infos;
	Infos.SetNumZeroed(Count);
	const int32 Got = FPlatformStackWalk::GetProcessModuleSignatures(Infos.GetData(), Count);

	Modules.Reserve(Got);
	for (int32 i = 0; i < Got; ++i)
	{
		const FStackWalkModuleInfo& M = Infos[i];
		if (M.BaseOfImage == 0 || M.ImageSize == 0) { continue; }

		FModuleRange R;
		R.Base = M.BaseOfImage;
		R.End  = M.BaseOfImage + M.ImageSize;
		R.Name = M.ModuleName;
		Modules.Add(MoveTemp(R));
	}

	// Sorted so the range test can stop early. With ~200 modules this is not a performance decision, it
	// is so the lookup is obviously correct rather than obviously fast.
	Modules.Sort([](const FModuleRange& A, const FModuleRange& B) { return A.Base < B.Base; });
}

const TCHAR* FFPMStallSampler::ModuleForAddress(uint64 Address) const
{
	for (const FModuleRange& R : Modules)
	{
		if (Address < R.Base) { break; }
		if (Address < R.End)  { return *R.Name; }
	}
	return nullptr;
}

void FFPMStallSampler::OnFrameBegin()
{
	LastFrameStart.store(FPlatformTime::Seconds(), std::memory_order_relaxed);
}

void FFPMStallSampler::Arm()
{
	if (Thread != nullptr) { return; }

	// Game thread, before the watchdog exists, so nothing can be reading it. See the header note on why
	// this must never be refreshed while a suspend is in flight.
	SnapshotModules();

	if (Modules.Num() == 0)
	{
		// ⚠ Stated, not silent. With no module table every sample would resolve to "unknown" and the
		// report would be a confident list of nothing. Better to refuse to arm and say why.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] stall sampler NOT armed - the process module table came back empty, so a captured "
			     "address could not be attributed to anything. No samples will be taken."));
		return;
	}

	LastFrameStart.store(FPlatformTime::Seconds(), std::memory_order_relaxed);
	FrameBeginHandle = FCoreDelegates::OnBeginFrame.AddRaw(this, &FFPMStallSampler::OnFrameBegin);

	bStopping.store(false, std::memory_order_relaxed);
	Thread = FRunnableThread::Create(this, TEXT("FPMStallSampler"), 0, TPri_BelowNormal);

	if (Thread == nullptr)
	{
		FCoreDelegates::OnBeginFrame.Remove(FrameBeginHandle);
		FrameBeginHandle.Reset();
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] stall sampler NOT armed - could not create its watchdog thread."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] stall sampler armed - samples the GAME THREAD's callstack when a frame passes %.0f ms, "
		     "up to %d time(s) this session, and reports WHICH MODULE it was inside. %d module(s) mapped. "
		     "The hitch meter can say a hitch is game-thread bound; only this can say what it was doing. "
		     "FPM.Stall.Report prints the ranking."),
		CVarStallSampleMs.GetValueOnAnyThread(), CVarStallSampleBudget.GetValueOnAnyThread(),
		Modules.Num());
}

void FFPMStallSampler::Stop()
{
	bStopping.store(true, std::memory_order_relaxed);
}

void FFPMStallSampler::Disarm()
{
	LogReport();

	// Unbind FIRST so the heartbeat stops moving, then join. A watchdog that wakes during teardown finds
	// bStopping set and exits without touching anything.
	if (FrameBeginHandle.IsValid())
	{
		FCoreDelegates::OnBeginFrame.Remove(FrameBeginHandle);
		FrameBeginHandle.Reset();
	}

	bStopping.store(true, std::memory_order_relaxed);
	if (Thread != nullptr)
	{
		// Kill(true) waits for Run() to return. Required, not optional: a sampler thread outliving this
		// object would suspend the game thread during shutdown and read a destroyed module table.
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
}

uint32 FFPMStallSampler::Run()
{
	while (!bStopping.load(std::memory_order_relaxed))
	{
		FPlatformProcess::Sleep(WatchdogTickMs / 1000.0f);
		if (bStopping.load(std::memory_order_relaxed)) { break; }

		const int32 Budget = CVarStallSampleBudget.GetValueOnAnyThread();
		if (Budget <= 0) { continue; }

		const double FrameStart = LastFrameStart.load(std::memory_order_relaxed);
		if (FrameStart <= 0.0) { continue; }

		const double Now       = FPlatformTime::Seconds();
		const double StalledMs = (Now - FrameStart) * 1000.0;
		if (StalledMs < CVarStallSampleMs.GetValueOnAnyThread()) { continue; }

		// ⚠ NEVER SAMPLE THE REPORT. LogReport emits two dozen log lines from the GAME THREAD, which can
		// itself pass the threshold — and a capture taken then would attribute the stall to this module.
		// An instrument must not appear in its own results.
		if (bReportInProgress.load(std::memory_order_acquire)) { continue; }

		// One sample per stall. Cleared when the game thread starts a new frame, below.
		if (bSampleInFlight.exchange(true, std::memory_order_acq_rel)) { continue; }

		const double GapMs = (Now - LastSampleAt.load(std::memory_order_relaxed)) * 1000.0;
		bool bOverBudget = false;
		{
			FScopeLock Lock(&ResultsLock);
			bOverBudget = SamplesTaken >= Budget;
		}

		if (!bOverBudget && GapMs >= CVarStallSampleGapMs.GetValueOnAnyThread())
		{
			SampleGameThread(StalledMs);
			LastSampleAt.store(Now, std::memory_order_relaxed);
		}

		// Wait for the game thread to move on before allowing another sample, so a 400 ms stall produces
		// one capture rather than twenty-six of the same callstack.
		while (!bStopping.load(std::memory_order_relaxed)
			&& LastFrameStart.load(std::memory_order_relaxed) == FrameStart)
		{
			FPlatformProcess::Sleep(WatchdogTickMs / 1000.0f);
		}
		bSampleInFlight.store(false, std::memory_order_release);
	}
	return 0;
}

void FFPMStallSampler::SampleGameThread(double StalledMs)
{
	const uint32 GameThreadId = GGameThreadId;
	if (GameThreadId == 0) { return; }

	uint64 BackTrace[MaxStackDepth];
	FMemory::Memzero(BackTrace, sizeof(BackTrace));

	// ⚠ THE SUSPEND HAPPENS INSIDE HERE. Nothing above or below it may take a lock the game thread could
	// be holding — that is the classic profiler deadlock. `Modules` is immutable after Arm and
	// `ResultsLock` is only ever taken by this thread and the report, never by the game thread.
	const uint32 Depth = FPlatformStackWalk::CaptureThreadStackBackTrace(
		GameThreadId, BackTrace, MaxStackDepth);

	FScopeLock Lock(&ResultsLock);
	++SamplesTaken;
	WorstSampledMs = FMath::Max(WorstSampledMs, StalledMs);

	if (Depth == 0)
	{
		// ⚠ A capture that returns nothing is a DEAD instrument, and it must be counted rather than
		// skipped. Otherwise a session where every capture failed reports an empty ranking that reads
		// exactly like a session with no stalls.
		++SamplesEmpty;
		return;
	}

	// Top-of-stack is where the thread actually IS. Anywhere-in-stack catches the case where a mod
	// called into the engine and the engine is the frame on top — the mod is still the reason.
	bool bTaggedTop = false;
	TSet<FString> SeenThisSample;
	for (uint32 i = 0; i < Depth; ++i)
	{
		if (BackTrace[i] == 0) { continue; }
		const TCHAR* Mod = ModuleForAddress(BackTrace[i]);
		const FString Name = Mod != nullptr ? FString(Mod) : FString(TEXT("<unmapped>"));

		if (!bTaggedTop)
		{
			ModuleHits.FindOrAdd(Name)++;
			bTaggedTop = true;
		}
		// Counted once per sample per module, so a deep recursion inside one module cannot outvote a
		// single frame in another.
		if (!SeenThisSample.Contains(Name))
		{
			SeenThisSample.Add(Name);
			ModuleAnywhere.FindOrAdd(Name)++;
		}
	}
}

void FFPMStallSampler::LogReport()
{
	// Held for the whole report. See the header note: without it, the report's own logging can overrun
	// the sample threshold and the sampler captures itself.
	bReportInProgress.store(true, std::memory_order_release);
	ON_SCOPE_EXIT { bReportInProgress.store(false, std::memory_order_release); };

	TArray<TPair<FString, int32>> Top;
	TArray<TPair<FString, int32>> Any;
	int32 Taken = 0;
	int32 Empty = 0;
	double Worst = 0.0;
	{
		FScopeLock Lock(&ResultsLock);
		Taken = SamplesTaken;
		Empty = SamplesEmpty;
		Worst = WorstSampledMs;
		for (const TPair<FString, int32>& P : ModuleHits)     { Top.Add(P); }
		for (const TPair<FString, int32>& P : ModuleAnywhere) { Any.Add(P); }
	}

	auto ByCountDesc = [](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{ return A.Value > B.Value; };
	Top.Sort(ByCountDesc);
	Any.Sort(ByCountDesc);

	/*
	 * ★ THE DENOMINATOR AND THE CAVEAT TRAVEL WITH THE NUMBERS, every time.
	 *
	 * "FicsitWiremod 14" is unreadable. "14 of 20 samples" is a finding. And because this samples only
	 * frames that ALREADY overran the threshold, these percentages are not a general profile of where
	 * time goes — they are a profile of where the game thread is WHEN IT IS STUCK. Someone will quote
	 * this at some point and the sentence has to survive the quoting.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] stall sampler: %d sample(s) taken (%d returned no frames), worst frame sampled "
		     "%.1f ms, threshold %.0f ms. These are samples of the game thread WHILE STALLED, not a "
		     "general profile."),
		Taken, Empty, Worst, CVarStallSampleMs.GetValueOnAnyThread());

	if (Taken == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   nothing sampled yet - no frame has passed %.0f ms since arming. That is a real "
			     "answer if the session was smooth, and a dead sampler if it was not."),
			CVarStallSampleMs.GetValueOnAnyThread());
		return;
	}

	const int32 ShowTop = FMath::Min(Top.Num(), 12);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   TOP OF STACK - what the game thread was executing:"));
	for (int32 i = 0; i < ShowTop; ++i)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]     %4d / %d  (%3.0f%%)  %s"),
			Top[i].Value, Taken, 100.0 * Top[i].Value / Taken, *Top[i].Key);
	}

	const int32 ShowAny = FMath::Min(Any.Num(), 12);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ANYWHERE IN STACK - who is on the callpath, including the caller that got us here:"));
	for (int32 i = 0; i < ShowAny; ++i)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]     %4d / %d  (%3.0f%%)  %s"),
			Any[i].Value, Taken, 100.0 * Any[i].Value / Taken, *Any[i].Key);
	}

	if (FPMDiag::IsOn(FPMDiag::EChannel::StallSampler) && Top.Num() > 0)
	{
		FPMOverlay::PostSticky(TEXT("stall sampler"), TEXT("top"),
			FString::Printf(TEXT("%d sample(s), worst %.0f ms | top: %s (%.0f%%)"),
				Taken, Worst, *Top[0].Key, 100.0 * Top[0].Value / Taken));
	}
}

/*
 * `FPM.Stall.Report` — the ranking on demand, because a stall you just felt is the moment you want it.
 */
static FAutoConsoleCommand GStallReportCmd(
	TEXT("FPM.Stall.Report"),
	TEXT("Print which modules the game thread was inside during measured stalls, ranked."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FFPMStallSampler::Get().LogReport();
	}));
