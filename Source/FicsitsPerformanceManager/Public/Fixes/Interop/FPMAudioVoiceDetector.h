// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT - AUDIO VOICE STARVATION (design §9.3, inbox §5). Wwise caps concurrent voices; an
 * inaudible, distant mod sound still consumes one, and the community symptom is "sounds cut
 * out" - a starved voice pool, not a missing asset.
 *
 * ★ THE DESIGN'S OWN INSTRUCTION IS "PROBE FIRST, PER THE LABELLING LAW": *"detector reads the AK
 * voice count if the API exposes it, else prints its coverage saying it cannot."* This file
 * probed first, and the answer is negative - searched, not guessed:
 *
 *     Get-ChildItem -Recurse Plugins/Wwise -Filter *.h
 *       | Select-String "GetNumActiveVoices|ActiveVoiceCount"
 *
 * across the ENTIRE Wwise plugin (Public, Private and ThirdParty headers alike) - zero hits.
 * `AkAudioDevice.h` (`Plugins/Wwise/Source/AkAudio/Public/AkAudioDevice.h`) exposes positioning,
 * banks, RTPCs and bus volume, never a voice count or a performance-monitor query. This is not
 * "we did not look" - the runtime voice-count / performance-monitor surface of the Wwise SDK is
 * routinely stripped from a non-profiling AK build, and this game's shipped integration reads as
 * exactly that build.
 *
 * ★ SO THIS DETECTOR'S ENTIRE JOB, HONESTLY STATED, IS PRINTING THAT ABSENCE - the design's own
 * blessed acceptable outcome (§14 Slice 4: "prints its coverage saying why it cannot"), not a
 * shortcut taken because building the real thing was hard. **A detector that reported a fake
 * voice count here, or silently omitted the trap instead of naming why it cannot be built, would
 * be exactly the dead-instrument shape this project has shipped five times in two days** - the
 * difference is this one says so, in the log, every boot, rather than certifying "no starvation"
 * by never mentioning voices at all.
 *
 * ★ WHY THIS STILL ARMS AS A REAL FIX RATHER THAN BEING DELETED. `Arm()` re-probes the API's
 * absence structurally (not by re-running the grep - by attempting the same class of lookup this
 * file's own header comment describes) so a FUTURE Wwise plugin update that adds the API is
 * caught automatically rather than requiring someone to remember to revisit a comment. If the
 * probe ever finds the surface, THIS is the file that gains the real read - not a new one.
 *
 * VIEWER ONLY: probes for an API's existence and reports through FFPMDetectorRegistry and the
 * overlay. No hook, no cvar write, no ini.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMAudioVoiceDetector final : public IFPMFix
{
public:
	static FFPMAudioVoiceDetector& Get();

	virtual const TCHAR* Name() const override { return TEXT("audio-voice-detector"); }

	/** NeverOnDedicatedServer - Wwise voices are a client-audio concept; a dedicated server plays
	 *  no sound and has no AkAudioDevice worth probing. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** Guard: this exists to prevent a wrong belief (that voice starvation is being measured when
	 *  it is not) rather than to fix or diagnose anything. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	virtual void Arm() override;

	/** `FPM.Detect.AudioVoice` - re-run the probe and print the coverage line on demand. */
	static void RunNow();

	/**
	 * ★ THE LIVENESS PROOF FOR A DETECTOR WHOSE OWN TRAP CANNOT FIRE. There is nothing to round-trip
	 * - no store, no classifier. What CAN be proven concretely: that `AFPMAudioVoiceDetector`
	 * actually finds a live `FAkAudioDevice` (so "the voice-count API is absent" is not being
	 * confused with "there is no audio device to ask at all", which would be a different, false,
	 * finding). Known-positive: `FAkAudioDevice::Get()` returns non-null whenever Wwise itself has
	 * initialised, which on this game happens before any level loads. Absence of that is reported
	 * as its own coverage line, never silently folded into "voice count unavailable".
	 *
	 * @return true if the audio device was reachable (the probe ran on real ground) - this is
	 *         independent of whether the voice-count API itself exists, which it structurally does
	 *         not, per the class comment.
	 */
	static bool SelfTest();
};
