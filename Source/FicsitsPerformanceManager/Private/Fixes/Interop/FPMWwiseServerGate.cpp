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
	StopActorHookHandle = FPM_SUBSCRIBE("wwise-server-gate", UAkGameplayStatics::StopActor,
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

void FFPMWwiseServerGate::Disarm()
{
	/*
	 * ★ THIS DID NOT EXIST, AND ITS ABSENCE MADE A TOGGLE LIE.
	 *
	 * `Arm()` discarded the `FPM_SUBSCRIBE` return value and no `Disarm()` was declared, so the base
	 * class's do-nothing body ran. `FPM.Fix.WwiseServerGate 0` and `FPM.Enabled 0` both reported success
	 * while the funchook detour stayed installed and the gate kept cancelling `StopActor`.
	 *
	 * That is worse than a missing feature. Ant used the per-fix toggles on 2026-08-11 to find which fix
	 * had broken her game — a toggle that silently does nothing would have cleared an innocent fix and
	 * sent the search somewhere else.
	 *
	 * ⚠ `UNSUBSCRIBE_METHOD` is correct even though this is the non-virtual `SUBSCRIBE_METHOD` form:
	 * both drive the same `HookInvoker<decltype(&M), &M>` and `RemoveHandler` clears the BEFORE and
	 * AFTER maps alike (`NativeHookManager.h:359-378`).
	 *
	 * ⚠ The `IsValid()` guard is load-bearing, not habit. On a client `Arm()` returns before subscribing
	 * at all, and in the editor the ledger refuses the install — both leave an invalid handle, and
	 * `RemoveHandler` would then walk arrays SML never allocated.
	 */
	if (StopActorHookHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UAkGameplayStatics::StopActor, StopActorHookHandle);
		StopActorHookHandle.Reset();
	}
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
