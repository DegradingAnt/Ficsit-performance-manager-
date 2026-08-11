// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ DLSS PRESET — the game pins Preset C, and C is the one that ghosts.
 *
 * Ant, 2026-08-11, describing the symptom before any of this was looked up: *"alot of ghosting is
 * particles and other stuff that move over other surfaces… it also has general ghosting aswell"*, and
 * *"id like to keep the performance of dlss and see if we can use a better pre-set to make it look less
 * bad."* This fix is that lever.
 *
 * ═══ WHAT THE GAME DOES, MEASURED ═══
 *
 * `FactoryGame.log:6048` — **`NGXDLSSPreset=Preset C(3)`**. Satisfactory asks for Preset C explicitly.
 * C is the old CNN-era preset; J and K are the transformer models, and their whole selling point is
 * exactly the artifact class she is describing.
 *
 * The lever, byte-verified out of the DLSS DLL's UTF-16 cvar table rather than taken from a wiki
 * (`CVAR-VERDICTS-2026-08-08.md:100`) — the help string reads:
 *
 *     "DLSS-SR/DLAA preset setting…  8,9: Unsupported preset / 10: Force preset J / 11: Force preset K"
 *
 * ⚠ THERE IS A DECOY. `r.NGX.DLSSRR.Preset` has a near-identical help string ("DLSS-RR/DLAA…", with
 * `8: H, 9: I`) and is the RAY RECONSTRUCTION preset, not super resolution. And `r.NGX.DLSS.DLSSPreset`
 * exists only as an ASCII error literal — `"Invalid r.NGX.DLSS.DLSSPreset value %d"` — never as a real
 * cvar. Both were checked; this fix targets `r.NGX.DLSS.Preset`.
 *
 * ═══ ★ THE OWNERSHIP HAZARD, WHICH IS WHY THIS NEEDED FPM AND NOT AN INI LINE ═══
 *
 * `CVAR-VERDICTS-2026-08-08.md:123` (R4) demoted this lever for a reason that has since been solved:
 * **FactoryGame owns this cvar.** In `FactoryGameSteam-FactoryGame-Win64-Shipping.dll` the string sits
 * in one contiguous pool with `UFGGameUserSettings`, `"Trying to use unsupported upscaling method %s.
 * Falling back to %s"`, `r.XeSS.Enabled` and `r.FidelityFX.FSR.Enabled` — i.e. the upscaler-switching
 * path. And `GameUserSettings.ini:13` proves FG persists raw cvars into `mIntValues`.
 *
 * So a naive write gets clobbered on the next settings Apply AND risks becoming a permanent ini entry.
 * Both halves are already handled here:
 *   · `FPMCVarWriter::Hold` writes at `ECVF_SetByPluginHighPriority` with FPM's tag, which outranks
 *     scalability and the options menu but stays BELOW the console, so the player keeps their console;
 *   · the write is RE-ASSERTED after `ApplyNonResolutionSettings`, the same way `FFPMGlassQuality`
 *     handles the identical problem;
 *   · `FFPMSaveSettingsInterceptor` stands FPM's holds down across `SaveSettings`, so nothing of ours
 *     is what FG serialises into `mIntValues`. That is the persistence half of R4, closed.
 *
 * ═══ WHAT THIS DOES NOT FIX, STATED PLAINLY ═══
 *
 * The particle-over-surface smear specifically is **not** a preset problem. It is the absence of a
 * reactive/bias mask: `Config/DefaultEngine.ini:1052` ships `r.FidelityFX.FSR3.CreateReactiveMask=False`
 * and CSS authored no DLSS stencil value, so no upscaler is told which pixels are reactive. A newer
 * preset reduces it; only a mask removes it, and that is separate work.
 *
 * ⚠ OFF BY DEFAULT, because which preset is BETTER is a visual judgement and hers to make. J and K
 * differ from each other and from C in ways no log can rank. `FPM.Upscaler.DLSSPreset` is the A/B.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMUpscalerPreset final : public IFPMFix
{
public:
	static FFPMUpscalerPreset& Get();

	virtual const TCHAR* Name() const override { return TEXT("upscaler-preset"); }

	/** Renderer-only. A dedicated server runs no upscaler and has no NGX at all. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * OriginNamed. The cause is not inferred: the game's own log line states the preset it requests, and
	 * the preset map came out of the DLL's cvar table. We are changing a stated choice, not guessing at
	 * a symptom.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::UpscalerPreset; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** Re-reads the requested preset once a world exists, and applies the hold if one is configured. */
	virtual void OnWorldLoad(UWorld* World) override;

	/**
	 * ⚠ REGISTERED BUT NOT ARMED BY DEFAULT — and unusually, that is not caution about the code.
	 *
	 * Which preset looks best is a VISUAL judgement on her screen, and C, J and K differ from each other
	 * in ways no counter can rank. Arming a default would be picking for her. `FPM.Upscaler.DLSSPreset`
	 * with a non-zero value is the opt-in, and `FPM.Upscaler.Report` states what the game asked for
	 * beside what we hold.
	 */
	virtual bool DefaultArmed() const override { return false; }

	/** `FPM.Upscaler.Report` — active upscaler, the preset the game asked for, and what we hold. */
	static void ReportNow();

private:
	/** Applies or releases the hold to match the cvar. Safe to call repeatedly; logs only on change. */
	void ApplyFromCVar(const TCHAR* Moment);

	FDelegateHandle ApplyHookHandle;

	/** Last value we successfully held, so a re-assert can be told from a first application. */
	int32 HeldPreset = 0;
};
