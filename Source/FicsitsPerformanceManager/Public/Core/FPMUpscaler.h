// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ WHICH UPSCALER IS ACTUALLY LIVE — the one question every upscaler lever has to answer first.
 *
 * Ant, 2026-08-11, on rebuilding FPM1's upscaler layer in FPM2: *"we need that back in fpm2"*. FPM1
 * carried `Res_UpscalerAutoSelect` — *"Tune for whichever upscaler is actually live (DLSS/XeSS/FSR/TSR)
 * instead of guessing by GPU vendor"* — and FPM2 shipped without it. This is that, rebuilt rather than
 * copied, and it is the piece the rest depends on.
 *
 * ═══ WHY IT IS SHARED INFRASTRUCTURE AND NOT A FIX ═══
 *
 * Same shape as `FPMEnclosure`: several consumers need the same answer, and each deriving it separately
 * is how three of them end up disagreeing. It holds NO POLICY — it reports what is live and never
 * decides what to do about it (`sf-scaffold` §3: nothing in `Core/` may include a feature header).
 *
 * ═══ HOW IT DECIDES, AND WHY NOT BY GPU VENDOR ═══
 *
 * ⚠ VENDOR IS THE WRONG QUESTION AND FPM1'S OWN SETTING TEXT SAYS SO. An NVIDIA card can be running
 * TSR, FSR or XeSS — the player picks, and a mod that infers "NVIDIA therefore DLSS" will hold a DLSS
 * preset that changes nothing while reporting success. That is the dead-instrument shape.
 *
 * The authoritative signal is FactoryGame's own selector, **`FG.UpScalingMethod`**, with `2 == DLSS`
 * confirmed independently twice from Ant's own config:
 *   · `FPM-AUDIT-FINDINGS-2026-07-24.md:35` — "FG.UpScalingMethod=2 (DLSS), r.AntiAliasingMethod=2"
 *   · `FPM-CVAR-RESEARCH…:373` — same, from her `GameUserSettings.ini`
 * and the banked governor rule (`…:450`) is stated in exactly those terms: *"every one of these is legal
 * only when the active upscaler is DLSS (`FG.UpScalingMethod == 2`)"*.
 *
 * ⚠ ONLY THE VALUE 2 IS VERIFIED. The other integers are NOT known — FG's own fallback string
 * (*"Trying to use unsupported upscaling method %s. Falling back to %s"*) proves the enum has more
 * members, and nothing read so far says which is which. So this reports DLSS or NOT-DLSS with
 * confidence and everything else as UNKNOWN, rather than inventing a mapping. The per-vendor enable
 * cvars (`r.XeSS.Enabled`, `r.FidelityFX.FSR.Enabled`) are read as CORROBORATION only, never as the
 * primary answer, because FG owns the switching path and a stale enable flag would mislead.
 */
enum class EFPMUpscaler : uint8
{
	Unknown,     ///< could not be determined — say so, never guess
	None,        ///< no temporal upscaler active (native / plain TAA)
	TSR,         ///< Unreal's own
	DLSS,        ///< FG.UpScalingMethod == 2, the one value that is verified
	FSR,
	XeSS,
};

class FICSITSPERFORMANCEMANAGER_API FPMUpscaler
{
public:
	/** The live upscaler, decided from FactoryGame's own selector first. Never guesses from GPU vendor. */
	static EFPMUpscaler Current();

	/** True only when the answer is DLSS and we are sure of it. The gate every DLSS lever must pass. */
	static bool IsDLSS();

	/** Human name for logs. Never returns an empty string. */
	static const TCHAR* NameOf(EFPMUpscaler Which);

	/**
	 * One line naming the live upscaler and the raw values it was derived from.
	 *
	 * ⚠ IT PRINTS THE INPUTS, not just the verdict. A verdict alone cannot be checked by the person
	 * reading a support dump, and this project has shipped a confident wrong verdict before.
	 */
	static FString Describe();
};
