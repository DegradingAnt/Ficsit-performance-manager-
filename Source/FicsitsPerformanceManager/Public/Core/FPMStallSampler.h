// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"

#include <atomic>

#include "Core/FPMFixContract.h"

class FRunnableThread;

/**
 * STALL SAMPLER — the one instrument that can NAME the game thread's missing 400 ms.
 *
 * ★ IT EXISTS BECAUSE THE HITCH METER RAN OUT OF THINGS TO SAY. From the 0.8.4 boot on Ant's own save,
 * 2026-08-10, primary:
 *     where: 12 game-thread bound, 0 render-thread bound, 0 neither (gpu/vsync/os)
 *     HITCH 414.5 ms | GAME-THREAD BOUND (game thread busy 413.7 ms, render thread completed 11.4 ms)
 *     HITCH 397.1 ms | GAME-THREAD BOUND (game thread busy 397.0 ms, ...)  | UNATTRIBUTED
 * 54 of 55 hitches game-thread bound, the game thread busy for ~99.9% of every span, and 83-100% of
 * them matching NONE of the six cause buckets. The meter has narrowed it as far as counting can: the
 * time is real, it is on the game thread, and no delegate this project can subscribe to explains it.
 *
 * Counting cannot go further. The next question is not "how many" but "doing WHAT", and only a sample
 * of the thread's own callstack answers that.
 *
 * ★ WHAT IT CAN ACTUALLY RESOLVE, AND WHY THAT IS ENOUGH. A retail install ships no PDBs for
 * FactoryGame or the engine, so function names are not available and this does not pretend otherwise.
 * It resolves each captured program counter to its OWNING MODULE by arithmetic — the module table gives
 * `BaseOfImage` and `ImageSize` (`GenericPlatformStackWalk.h:20-27`) and an address either falls inside
 * a range or it does not. No symbol server, no dbghelp lookup, nothing that can fail quietly.
 *
 * With 53 mods on Ant's server and 124 in her client profile, "14 of 20 stall samples were inside
 * FactoryGame-FicsitWiremod.dll" is not a partial answer. It is the answer.
 *
 * ★ HOW IT AVOIDS BEING A PROFILER THAT CAUSES THE PROBLEM IT MEASURES.
 *  - The game thread's only cost is one atomic store per frame. Everything else runs on this thread.
 *  - `CaptureThreadStackBackTrace` briefly SUSPENDS the game thread, so it fires at most ONCE per stall
 *    and never while a sample is already in flight.
 *  - A session budget caps the total. An instrument that suspends the game thread ten thousand times is
 *    a bug, so the cap is a hard number and the count is reported beside the results.
 *  - The module table is snapshotted at Arm on the game thread and never re-read while a suspend is in
 *    progress. Taking a lock that the suspended thread might hold is the classic profiler deadlock, and
 *    the snapshot is what makes it impossible here rather than merely unlikely.
 *
 * ⚠ IT SAMPLES STALLS, NOT FRAMES, AND THAT IS A REAL LIMIT. A profiler samples continuously and builds
 * a distribution over all execution. This fires only when a frame has already overrun the threshold, so
 * it answers "what is the game thread inside WHEN IT IS STUCK" and says nothing about steady-state cost.
 * That is the question Ant asked, but the distinction has to survive into the report or someone will
 * read these percentages as a general profile.
 *
 * VIEWER ONLY: no hook, no cvar write, no ini, no network. It reads a clock, suspends a thread for
 * microseconds, and counts.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMStallSampler final : public IFPMFix, public FRunnable
{
public:
	static FFPMStallSampler& Get();

	virtual const TCHAR* Name() const override { return TEXT("stall-sampler"); }

	/**
	 * Both sides. The 560 ms save-serialisation stall lives on the SERVER, and a server has no overlay
	 * and no `stat` readout — a module name in the log is the only instrument that reaches it at all.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * UnknownCause, and emphatically so. This fix exists precisely because the cause is not known; the
	 * day it names one, the fix that ACTS on that cause is a different fix with a different status.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::StallSampler; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.Stall.Report` — modules ranked by how often the game thread was inside them mid-stall. */
	void LogReport();

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	/** Stamped from the game thread once per frame. The only cost this fix imposes on it. */
	void OnFrameBegin();

	/** Snapshot the loaded-module table. Game thread, at Arm, never while a suspend is in flight. */
	void SnapshotModules();

	/** Address -> module name by range test. Returns nullptr when no module owns it. */
	const TCHAR* ModuleForAddress(uint64 Address) const;

	/** One capture of the game thread's stack, attributed to modules. Watchdog thread only. */
	void SampleGameThread(double StalledMs);

	struct FModuleRange
	{
		uint64  Base = 0;
		uint64  End  = 0;
		FString Name;
	};

	/** Immutable after Arm, which is what makes it safe to read while the game thread is suspended. */
	TArray<FModuleRange> Modules;

	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopping{false};

	/** Seconds, from FPlatformTime::Seconds(). Written by the game thread, read by the watchdog. */
	std::atomic<double> LastFrameStart{0.0};

	/** Guards re-sampling the same stall over and over while it is still going. */
	std::atomic<bool> bSampleInFlight{false};
	std::atomic<double> LastSampleAt{0.0};

	FDelegateHandle FrameBeginHandle;

	/**
	 * Results. Guarded by a lock the GAME THREAD never takes while suspended — only the watchdog writes,
	 * and only the report reads, so the lock can never be held by the thread we are about to suspend.
	 */
	mutable FCriticalSection ResultsLock;
	TMap<FString, int32> ModuleHits;      // top-of-stack attribution
	TMap<FString, int32> ModuleAnywhere;  // anywhere-in-stack attribution
	int32 SamplesTaken = 0;
	int32 SamplesEmpty = 0;              // captures that returned no frames — a dead capture, reported
	double WorstSampledMs = 0.0;
};
