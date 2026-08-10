// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMWwiseServerGate.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "AkGameplayStatics.h"

#include <atomic>

namespace
{
	/**
	 * Atomic because the FactoryGame cleanup paths that call StopActor are not ours to make promises
	 * about, and a counter costs nothing next to the call it is replacing.
	 */
	std::atomic<int64> GFPMWwiseSuppressed{0};

	/**
	 * One line per 100,000 suppressions. Sparse ON PURPOSE — a gate whose whole job is to stop writing
	 * a line per call must not write a line per call to say so. At the measured rate (681 in a ~111
	 * minute session) this heartbeat would fire roughly never, which is the correct volume for
	 * "working as intended"; the arm line is what proves it is live.
	 */
	constexpr int64 HeartbeatEvery = 100000;
}

FFPMWwiseServerGate& FFPMWwiseServerGate::Get()
{
	static FFPMWwiseServerGate Instance;
	return Instance;
}

int64 FFPMWwiseServerGate::SuppressedCount()
{
	return GFPMWwiseSuppressed.load(std::memory_order_relaxed);
}

void FFPMWwiseServerGate::Arm()
{
	/*
	 * ★ THE GUARD IS AT REGISTRATION, NOT PER CALL. On a client the audio device is real and StopActor
	 * genuinely stops sounds — cancelling it there would be an audio bug, not an optimisation. Bailing
	 * here means a client installs NO HOOK AT ALL, so there is no per-call branch that a later edit
	 * could get wrong, and no cost on the machine that does not need the fix.
	 */
	if (!IsRunningDedicatedServer())
	{
		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::WwiseGate, 2), LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] Wwise server gate: not a dedicated server, no hook installed (correct - this "
			     "machine has a real audio device and StopActor must run)."));
		return;
	}

	/*
	 * Plain SUBSCRIBE_METHOD, not the _VIRTUAL form: `StopActor` is a static BlueprintCallable on
	 * UAkGameplayStatics (`AkGameplayStatics.cpp:966`), not a virtual, so there is no vtable to read
	 * and no sample object to read it from.
	 *
	 * Unconditional cancel is safe HERE and only here: we already know from the source that the very
	 * first thing the original does on this machine is fail `FAkAudioDevice::Get()` and return. There
	 * is no branch in it that could do something useful on a server.
	 */
	FPM_SUBSCRIBE("wwise-server-gate", UAkGameplayStatics::StopActor,
		[](auto& Scope, AActor* /*Actor*/)
		{
			Scope.Cancel();

			const int64 Count = GFPMWwiseSuppressed.fetch_add(1, std::memory_order_relaxed) + 1;
			if (Count == 1 || (Count % HeartbeatEvery) == 0)
			{
				UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::WwiseGate), LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] Wwise server gate: %lld StopActor no-op(s) suppressed this session."),
					static_cast<long long>(Count));
			}
		});

	// Not gated by the channel: the stated Arm()-line exception in FPMDiag.h. This is the line that
	// separates "suppressed nothing because the gate is working" from "never armed".
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] Wwise server audio gate armed (dedicated server). StopActor cannot reach an audio "
		     "device here (AkGameplayStatics.cpp:974-979), so cancelling it is behaviour-identical "
		     "minus the log write. Measured 681 such warnings in the 2026-08-09 server session."));
}

/*
 * `FPM.WwiseGate.Report` — the count, on demand, with the context that makes it mean something.
 *
 * On a client this correctly reports 0 and says why, rather than looking like a broken gate.
 */
static FAutoConsoleCommand GWwiseGateReportCmd(
	TEXT("FPM.WwiseGate.Report"),
	TEXT("Print how many Wwise StopActor no-ops the server gate has suppressed this session."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		if (!IsRunningDedicatedServer())
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] Wwise server gate: not armed on this machine - it is client-side only by "
				     "design, because a client's audio device is real."));
			return;
		}
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] Wwise server gate: %lld StopActor no-op(s) suppressed this session."),
			static_cast<long long>(FFPMWwiseServerGate::SuppressedCount()));
	}));
