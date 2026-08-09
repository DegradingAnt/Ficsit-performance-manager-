// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ ZIPLINE VOLUME — the one audio lever in FPM. Design P3.9.
 *
 * Ant, 2026-08-02: *"ziplines are very load, they need a volume setting"*, and on the vanilla options:
 * *"It ships a volume slider but not for just the zipline."*
 *
 * ★ WHY NOT THE TWO OBVIOUS ROUTES — both were checked on the old mod and both are dead ends. Carried
 * because re-deriving them costs an evening:
 *
 *   1. `US_ZiplineVolume` EXISTS as an asset, but it is FGGameUserSettings-backed, so anything written
 *      to it is re-applied on every boot WITH OR WITHOUT this mod installed. That is permanent residue,
 *      forbidden outright. Its existence in the table is also NOT evidence that a slider is exposed —
 *      Ant confirmed from the live game that there is no per-zipline slider.
 *   2. A Wwise BUS. There is none to use: the mixer hierarchy is
 *      `Master_Audio_Bus -> gameMix -> _reverbSends`, with no per-system bus and no zipline RTPC.
 *      Lowering gameMix quiets the whole game, which is what the vanilla slider already does and what
 *      Ant said does not solve her problem.
 *
 * WHAT ACTUALLY MAKES THE NOISE: `Play_Zipline_Travelling` posts two LOOPING sources —
 * `Equipment_Zipline_InterferenceLoop.wav` and `Equipment_Zipline_Wind_01.wav`. (The `HAP_*` entries in
 * that event are haptics — controller rumble, not audio. Do not chase them.)
 *
 * THE HANDLE IS PER-ACTOR: `UAkGameplayStatics::SetOutputBusVolume(float, AActor*)`. Scoping to the
 * equipment ACTOR is deliberate — an earlier draft enumerated UAkComponents and matched
 * `mZIplineTravellingSFX` by name, including CSS's own typo with a capital "I", which would have gone
 * silently inert the day they renamed it. Ant pushed back on that twice: *"cant we just attack the
 * sound output?"* The actor-scoped call is smaller and cannot rot that way.
 *
 * ⚠ SET ON EQUIP, NOT LIVE. With no RTPC there is nothing to drive continuously, so the value lands
 * when the zipline is equipped and a mid-ride change takes effect on the next equip.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHAT CHANGED IN THE PORT — this is not a copy.
 *
 * 1. **A REAL BUG IS FIXED.** The old version returned early whenever the configured volume was
 *    `>= 0.999f`, on the reasoning that vanilla must write nothing. Correct for a fresh session — and
 *    wrong the moment the value comes BACK. Set 0.30, ride, then set it back to 1.00: the guard skips
 *    the write, so the bus stays at 0.30 and vanilla is unreachable until the game restarts. This
 *    version remembers whether it has ever written, and once it has, it writes every time INCLUDING
 *    1.0 — so "put it back" actually puts it back.
 *
 * 2. **The value comes from FPM's OWN cvar**, `FPM.Zipline.Volume`, not from a config struct. FPM2 has
 *    no settings surface until Phase 4, and a cvar we declare ourselves is neither a US_*-backed write
 *    nor an ini write, so it carries no residue. When Phase 4 lands, the setting drives this cvar and
 *    nothing here changes.
 *
 * 3. **The side gate is declared, not hand-rolled.** The old file early-returned on
 *    `IsRunningDedicatedServer()`. Declaring `NeverOnDedicatedServer` gets the same protection AND logs
 *    the skip, so a server log can tell "gated off" from "never armed".
 */
class FFPMZiplineVolume final : public IFPMFix
{
public:
	static FFPMZiplineVolume& Get();

	virtual const TCHAR* Name() const override { return TEXT("zipline-volume"); }

	/** A dedicated server has no audio device, so every Ak call there is a guaranteed no-op that still
	 *  costs a hook into a separately compiled Linux binary. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * OriginNamed: the cause is not a bug at all, it is that vanilla ships no per-zipline slider while
	 * the event posts two looping sources. We know exactly what makes the noise and exactly which handle
	 * changes it. This is the rare case where "fixed" would be the honest word.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Zipline; }

	virtual void Arm() override;

	/** Equips seen, and writes actually issued. A zero write count with a non-zero equip count means the
	 *  lever is sitting at vanilla — which is the correct default, not a fault. */
	static void GetCounts(int32& OutEquips, int32& OutWrites);
};
