// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMUpscaler.h"

/**
 * ★ SHARPENING, ROUTED BY WHICHEVER UPSCALER IS ACTUALLY LIVE.
 *
 * Carried back from FPM1, which shipped two separate knobs for this and the rewrite dropped both:
 *   `Res_FSRSharpness`   — *"FSR's own RCAS sharpener, applied only when FSR is the live upscaler."*
 *   `Res_TsrXessSharpen` — *"Tonemapper sharpen on TSR/no-upscaler paths (never stacked on
 *                           DLSS/XeSS/FSR)."*
 *
 * ═══ ★ THIS IS WHAT MAKES `FPMUpscaler` LOAD-BEARING ═══
 *
 * The probe shipped in 0.11.21 and, until now, nothing ACTED on it — it reported and no one listened,
 * which is a dead instrument wearing a useful name. This fix is its first real consumer, and the
 * routing is the whole point: **the two sharpeners must never both apply.**
 *
 * FSR has its own RCAS pass. DLSS and XeSS do their own sharpening internally. The tonemapper's
 * sharpen is a separate, always-available pass. Stack the tonemapper on top of an upscaler that
 * already sharpened and the result is over-sharpened ringing on every edge — which reads as "the mod
 * made my game look worse" and is exactly the trap FPM1's own setting text warns about:
 * *"never stacked on DLSS/XeSS/FSR"*.
 *
 * So: FSR gets `r.FidelityFX.FSR.Sharpness`, TSR and native get `r.Tonemapper.Sharpen`, and DLSS and
 * XeSS get **nothing at all** — they sharpen themselves and there is no correct value for us to add.
 *
 * ═══ THE CVAR NAMES ARE READ, NOT INHERITED ═══
 *
 * FPM1's strings were treated as a hypothesis and checked against the shipped bytes 2026-08-11:
 *   · `r.FidelityFX.FSR.Sharpness` — extracted from
 *     the FSR plugin's shipped DLLs under `FactoryGame/Plugins/FSR/Binaries/Win64`.
 *   · `r.Tonemapper.Sharpen` — `PostProcessTonemap.cpp:36-41`, default **-1**, help text:
 *     *"<0: inherit from PostProcessVolume settings (default) / 0: off"*.
 *
 * ⚠ THAT DEFAULT OF -1 IS WHY THE OFF VALUE HERE IS -1 AND NOT 0. Writing 0 would not be "leave it
 * alone", it would be *turning off* a sharpen the level's own PostProcessVolume may have asked for —
 * a change disguised as a neutral default.
 *
 * ⚠ AND `r.FidelityFX.FSR.Sharpness` IS ONLY MEANINGFUL WHEN FSR IS RUNNING. Board m5664350 records
 * FSR shipping BROKEN on Ant's stack — green chroma corruption with a hard diagonal boundary — so this
 * path is the least exercised of the three and says so in its own report rather than pretending
 * otherwise.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMUpscalerSharpness final : public IFPMFix
{
public:
	static FFPMUpscalerSharpness& Get();

	virtual const TCHAR* Name() const override { return TEXT("upscaler-sharpness"); }

	/** Purely a rendering concern. A dedicated server has no tonemapper and no upscaler. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * ModFeature territory rather than a repair: nothing is broken. This adds a control the game does
	 * not expose, and routes it so it cannot be applied twice.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Sharpness; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** Re-routes on every world load, because the player can change upscaler between sessions. */
	virtual void OnWorldLoad(UWorld* World) override;

	/**
	 * Off until asked for. Sharpening is a look preference with no correct default, and the values
	 * FPM1 shipped (0.8 tonemapper, 0.5 FSR) were never measured against anything — they were someone's
	 * taste, and inheriting them as a default would be presenting taste as a fix.
	 */
	virtual bool DefaultArmed() const override { return false; }

	/** `FPM.Sharpness.Report` — the live upscaler, which lever that selects, and what is held. */
	static void ReportNow();

private:
	/** Applies the right lever for the current upscaler and releases the other. */
	void Route(const TCHAR* Moment);

	/** Which lever is currently held, so a re-route can release the one that no longer applies. */
	EFPMUpscaler RoutedFor = EFPMUpscaler::Unknown;
};
