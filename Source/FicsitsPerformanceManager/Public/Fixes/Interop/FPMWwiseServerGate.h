// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

#include "Core/FPMFixContract.h"

/**
 * WWISE SERVER AUDIO GATE — stop a call that cannot possibly do anything from writing a log line each
 * time it fails.
 *
 * ★ THE ORIGIN IS NAMED AND READ FROM THE SOURCE, not inferred. `UAkGameplayStatics::StopActor` opens
 * with a device fetch and bails when it comes back null (`AkGameplayStatics.cpp:966-979`):
 *
 *     FAkAudioDevice * AudioDevice = FAkAudioDevice::Get();
 *     if (UNLIKELY(!AudioDevice))
 *     {
 *         UE_LOG(LogAkAudio, Warning, TEXT("UAkGameplayStatics::StopActor: Could not retrieve audio device."));
 *         return;
 *     }
 *
 * A dedicated server has no audio device, so on that machine this function is a GUARANTEED no-op whose
 * only observable effect is the warning. Cancelling it there is behaviour-identical minus the log
 * write — not a suppression of information, because there is no information: the call stopped nothing
 * either way.
 *
 * ★ MEASURED, 2026-08-09 server session (19:51:52 → 21:42:41 local, the one that crashed): **681**
 * occurrences of that warning. It is one of three repeating lines that together are 23% of a
 * 61,687-line log — and a log you have to wade through is a log that hides the crash callstack you
 * were actually looking for. That is the real cost here, more than the CPU.
 *
 * ⚠ CLIENTS ARE NOT TOUCHED, and the guard is registration-time rather than per-call. A client's audio
 * device is real, `StopActor` genuinely stops sounds, and cancelling it would be an audio bug. `Arm()`
 * returns early on anything that is not a dedicated server, so on a client this fix installs no hook at
 * all — there is no per-call branch to get wrong later.
 *
 * ★ WHY `Side()` IS `Any` DESPITE BEING SERVER-ONLY. `EFPMFixSide` offers `Any` and
 * `NeverOnDedicatedServer`; there is no server-only value, and the enum's own note says a fix that
 * needs one side "self-guards on authority if it needs to". `NeverOnDedicatedServer` would be exactly
 * backwards here. So: `Any`, with the self-guard in `Arm()`.
 *
 * ⚠ THIS IS A RE-PORT OF A FIX THE REWRITE DROPPED. FPM1 had it
 * (`FicsitPerformanceManager.cpp:1480-1510`, `RegisterWwiseServerAudioGate`); FPM2 shipped without it
 * and nothing noticed until its warnings turned up while reading an unrelated crash log. That is the
 * third fix this rewrite has orphaned. REBUILT here rather than copied, per this project's convention,
 * and its central claim was re-verified from the Wwise source rather than trusted from the old
 * comment — the old comment cited 3,164 warnings/session, which was ITS session, not this one.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMWwiseServerGate final : public IFPMFix
{
public:
	static FFPMWwiseServerGate& Get();

	virtual const TCHAR* Name() const override { return TEXT("wwise-server-gate"); }

	/** Any + a self-guard in Arm(). See the header note — there is no server-only side value. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * OriginNamed: the cause is not a hypothesis. The null-device early-out is in the Wwise source at
	 * `AkGameplayStatics.cpp:974-979` and was read there, so we know exactly why the call is pointless
	 * on this machine and exactly what cancelling it costs, which is nothing.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::WwiseGate; }

	virtual void Arm() override;

	/** Suppressed-call total for the session. Read by `FPM.WwiseGate.Report` and the overlay. */
	static int64 SuppressedCount();
};
