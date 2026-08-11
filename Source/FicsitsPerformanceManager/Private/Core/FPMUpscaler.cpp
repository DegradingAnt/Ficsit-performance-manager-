// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMUpscaler.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * FactoryGame's own upscaler selector. THE authoritative signal — see the header for why vendor is
	 * the wrong question.
	 */
	const TCHAR* GFPMUpscalerSelector = TEXT("FG.UpScalingMethod");

	/** The ONE value that is verified, from Ant's own GameUserSettings twice over. */
	constexpr int32 GFPMUpScalingMethodDLSS = 2;

	int32 ReadInt(const TCHAR* Name, int32 Fallback)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetInt() : Fallback;
	}

	/** Present-and-enabled, for corroboration only. -1 means the cvar does not exist on this build. */
	int32 EnableFlag(const TCHAR* Name) { return ReadInt(Name, -1); }
}

EFPMUpscaler FPMUpscaler::Current()
{
	const int32 Method = ReadInt(GFPMUpscalerSelector, -1);

	/*
	 * ★ THE VERIFIED ANSWER FIRST, AND IT IS THE ONLY ONE STATED WITH CONFIDENCE.
	 * FG.UpScalingMethod == 2 is DLSS, confirmed independently from her config twice. No other integer
	 * in this enum has been read anywhere, and inventing a mapping is how a probe starts lying.
	 */
	if (Method == GFPMUpScalingMethodDLSS)
	{
		return EFPMUpscaler::DLSS;
	}

	/*
	 * NOT DLSS. Fall back to the per-vendor enable flags — but ONLY to name which of the others it is,
	 * never to contradict the selector above. FG owns the switching path, so an enable flag left set
	 * from a previous choice is a real possibility and must not outrank FG's own answer.
	 */
	if (EnableFlag(TEXT("r.XeSS.Enabled")) > 0)            { return EFPMUpscaler::XeSS; }
	if (EnableFlag(TEXT("r.FidelityFX.FSR3.Enabled")) > 0) { return EFPMUpscaler::FSR; }
	if (EnableFlag(TEXT("r.FidelityFX.FSR.Enabled")) > 0)  { return EFPMUpscaler::FSR; }

	/*
	 * TSR is Unreal's own and is selected through r.AntiAliasingMethod == 4 (AAM_TSR). That constant is
	 * engine-stable, unlike FG's selector enum.
	 */
	if (ReadInt(TEXT("r.AntiAliasingMethod"), -1) == 4)    { return EFPMUpscaler::TSR; }

	/*
	 * ⚠ UNKNOWN IS A REAL ANSWER AND MUST NOT COLLAPSE INTO "None".
	 * If the selector cvar was missing entirely (Method == -1) we know nothing — that is different from
	 * knowing there is no upscaler, and a consumer gating on "not DLSS" would treat them identically to
	 * its cost. Say which.
	 */
	if (Method < 0)
	{
		return EFPMUpscaler::Unknown;
	}

	return EFPMUpscaler::None;
}

bool FPMUpscaler::IsDLSS()
{
	return Current() == EFPMUpscaler::DLSS;
}

const TCHAR* FPMUpscaler::NameOf(EFPMUpscaler Which)
{
	switch (Which)
	{
	case EFPMUpscaler::None:    return TEXT("none (native/TAA)");
	case EFPMUpscaler::TSR:     return TEXT("TSR");
	case EFPMUpscaler::DLSS:    return TEXT("DLSS");
	case EFPMUpscaler::FSR:     return TEXT("FSR");
	case EFPMUpscaler::XeSS:    return TEXT("XeSS");
	case EFPMUpscaler::Unknown: return TEXT("UNKNOWN");
	default:                    return TEXT("UNKNOWN");
	}
}

FString FPMUpscaler::Describe()
{
	/*
	 * PRINT THE INPUTS BESIDE THE VERDICT. A verdict on its own cannot be checked by whoever reads a
	 * support dump, and this project has shipped a confident wrong verdict before. Anyone can now
	 * disagree with the conclusion using the same numbers it was drawn from.
	 */
	return FString::Printf(
		TEXT("upscaler = %s  [FG.UpScalingMethod=%d (2=DLSS, others UNVERIFIED) · r.AntiAliasingMethod=%d "
		     "(4=TSR) · r.NGX.DLSS.Enable=%d · r.XeSS.Enabled=%d · r.FidelityFX.FSR3.Enabled=%d · "
		     "r.FidelityFX.FSR.Enabled=%d ; -1 = cvar absent]"),
		NameOf(Current()),
		ReadInt(GFPMUpscalerSelector, -1),
		ReadInt(TEXT("r.AntiAliasingMethod"), -1),
		EnableFlag(TEXT("r.NGX.DLSS.Enable")),
		EnableFlag(TEXT("r.XeSS.Enabled")),
		EnableFlag(TEXT("r.FidelityFX.FSR3.Enabled")),
		EnableFlag(TEXT("r.FidelityFX.FSR.Enabled")));
}
