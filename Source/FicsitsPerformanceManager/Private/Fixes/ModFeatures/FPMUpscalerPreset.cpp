// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMUpscalerPreset.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMUpscaler.h"
#include "Core/FPMHookLedger.h"

#include "FGGameUserSettings.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/** The cvar FactoryGame also writes. See the header's R4 note for why that matters. */
	const TCHAR* GFPMPresetCVar = TEXT("r.NGX.DLSS.Preset");

	/** Owner name in the writer's ledger, so FPM.Changes attributes the hold to this fix. */
	const FName GFPMPresetOwner(TEXT("upscaler-preset"));

	/*
	 * ★ THE VALUE MAP, TAKEN FROM THE DLL'S OWN HELP STRING, NOT FROM A WIKI.
	 * Extracted UTF-16 from FactoryGameSteam-DLSS-Win64-Shipping.dll (CVAR-VERDICTS-2026-08-08.md:100):
	 *     "DLSS-SR/DLAA preset setting… 8,9: Unsupported preset / 10: Force preset J / 11: Force preset K"
	 * 0 leaves the game's own choice alone, which the game logs as Preset C(3).
	 */
	TAutoConsoleVariable<int32> CVarDLSSPreset(
		TEXT("FPM.Upscaler.DLSSPreset"), 0,
		TEXT("Force a DLSS super-resolution preset. 0 = leave the game's choice alone (it asks for "
		     "Preset C, which is the old CNN model and the one that ghosts). 10 = force preset J, "
		     "11 = force preset K - both transformer models. 8 and 9 are UNSUPPORTED per the DLL's own "
		     "help text. Which of J or K looks better is a visual judgement; this is the A/B."),
		ECVF_Default);

	/** Highest value the DLL's help text documents. Anything above is refused rather than passed through. */
	constexpr int32 GFPMPresetMax = 11;

	/*
	 * What the game asked for, captured once so the report can state the BEFORE alongside our hold.
	 * -1 = never read. The game logs "NGXDLSSPreset=Preset C(3)" but that is a log line, not an API, so
	 * this reads the live cvar instead.
	 */
	int32 GFPMPresetGameRequested = -1;

	int32 ReadLiveCVar(const TCHAR* Name, int32 Fallback = -1)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetInt() : Fallback;
	}

	/**
	 * Is DLSS actually the live upscaler?
	 *
	 * ⚠ THIS IS THE LIVENESS QUESTION FOR THE WHOLE FIX. Holding a DLSS preset while the player is on
	 * TSR, FSR or XeSS changes nothing and would report success forever - the dead-instrument shape.
	 *
	 * ★ IT ASKS FPMUpscaler RATHER THAN READING r.NGX.DLSS.Enable DIRECTLY, and that is a correction.
	 * The first version of this file read the enable flag, which is an ARTEFACT of FG's switching path,
	 * not the choice itself - a flag left set from a previous selection would answer YES for an upscaler
	 * that is no longer live. FG.UpScalingMethod is the selector, and 2 == DLSS is the one value verified
	 * from Ant's own config twice over.
	 */
	bool IsDLSSLive()
	{
		return FPMUpscaler::IsDLSS();
	}
}

FFPMUpscalerPreset& FFPMUpscalerPreset::Get()
{
	static FFPMUpscalerPreset Instance;
	return Instance;
}

void FFPMUpscalerPreset::Arm()
{
	/*
	 * ★ RE-ASSERT AFTER THE OPTIONS MENU, because FactoryGame owns this cvar.
	 *
	 * R4's hazard: the string lives in FG's upscaler-switching path beside `r.XeSS.Enabled` and
	 * `r.FidelityFX.FSR.Enabled`, so any settings Apply can rewrite it. `FFPMGlassQuality` already
	 * solved the identical problem on the identical hook, so this follows that shape rather than
	 * inventing a second one.
	 *
	 * _AFTER, not before: we want to write once FG has finished writing, not race it.
	 */
	UFGGameUserSettings* Sample = GetMutableDefault<UFGGameUserSettings>();

	auto OnApplied = [](UFGGameUserSettings* /*Self*/)
	{
		FFPMUpscalerPreset::Get().ApplyFromCVar(TEXT("after settings apply"));
	};

	ApplyHookHandle = FPM_SUBSCRIBE_VIRTUAL_AFTER("upscaler-preset",
		UFGGameUserSettings::ApplyNonResolutionSettings, Sample, OnApplied);

	/*
	 * React to the player changing the cvar live, so an A/B needs no reload. The callback fires on the
	 * game thread from the console, which is where every write below belongs.
	 */
	CVarDLSSPreset.AsVariable()->SetOnChangedCallback(
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
		{
			FFPMUpscalerPreset::Get().ApplyFromCVar(TEXT("cvar changed"));
		}));

	// Ungated by the diag channel: the stated Arm()-line exception. It separates "held nothing because
	// the player asked for nothing" from "never armed".
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] upscaler preset ARMED. Satisfactory asks NGX for Preset C (its own log line reads "
		     "'NGXDLSSPreset=Preset C(3)'), the old CNN model and the one that ghosts on movers. "
		     "FPM.Upscaler.DLSSPreset 10 forces preset J, 11 forces K - both transformer models. The "
		     "write goes through FPMCVarWriter at plugin-high priority so the options menu cannot "
		     "clobber it and the save interceptor keeps it out of GameUserSettings.ini, which is the "
		     "ownership hazard that kept this lever unbuilt."));

	ApplyFromCVar(TEXT("arm"));
}

void FFPMUpscalerPreset::Disarm()
{
	/*
	 * Release the hold FIRST, then drop the hook. The other order would leave the re-assert able to fire
	 * once more between the two and put back a value we just said we had released — which is exactly the
	 * ordering bug the master switch's DisarmAll/ReleaseAll sequence exists to avoid.
	 */
	FPMCVarWriter::Get().ReleaseOwner(GFPMPresetOwner);
	HeldPreset = 0;

	if (ApplyHookHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGGameUserSettings::ApplyNonResolutionSettings, ApplyHookHandle);
		ApplyHookHandle.Reset();
	}
}

void FFPMUpscalerPreset::OnWorldLoad(UWorld* /*World*/)
{
	/*
	 * Capture what the game settled on, ONCE, before we ever hold anything. Arm() runs at module
	 * startup, which is before FG has configured the upscaler, so the value read there is not the
	 * game's answer — a report built on it would state the wrong BEFORE.
	 */
	if (GFPMPresetGameRequested < 0)
	{
		GFPMPresetGameRequested = ReadLiveCVar(GFPMPresetCVar);
	}

	ApplyFromCVar(TEXT("world load"));
}

void FFPMUpscalerPreset::ApplyFromCVar(const TCHAR* Moment)
{
	const int32 Want = CVarDLSSPreset.GetValueOnGameThread();

	// 0 = hands off. Release anything we hold and let the game's own choice stand.
	if (Want == 0)
	{
		if (HeldPreset != 0)
		{
			FPMCVarWriter::Get().Release(GFPMPresetOwner, GFPMPresetCVar);
			HeldPreset = 0;
			UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::UpscalerPreset), LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] upscaler preset: released at %s - the game's own preset is back in charge."),
				Moment);
		}
		return;
	}

	/*
	 * ⚠ REFUSE 8 AND 9 RATHER THAN PASS THEM THROUGH. The DLL's own help says "8,9: Unsupported preset".
	 * Writing an unsupported value would either be ignored or fall back silently, and either way the
	 * player would have set a preset and got something else while everything reported success.
	 */
	if (Want == 8 || Want == 9 || Want < 0 || Want > GFPMPresetMax)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] upscaler preset: REFUSING FPM.Upscaler.DLSSPreset=%d. The DLSS DLL's own help "
			     "documents 8 and 9 as UNSUPPORTED and nothing above %d. Use 10 (preset J) or 11 "
			     "(preset K), or 0 to leave the game's choice alone."), Want, GFPMPresetMax);
		return;
	}

	/*
	 * ★ SAY SO WHEN DLSS IS NOT THE LIVE UPSCALER. Holding a DLSS preset under TSR/FSR/XeSS changes
	 * nothing at all, and without this line the fix would report a successful hold forever while doing
	 * exactly nothing — the dead-instrument shape. The hold still happens, because the player may switch
	 * to DLSS without reloading and the value should already be right when they do.
	 */
	UE_CLOG(!IsDLSSLive(), LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] upscaler preset: DLSS is NOT the active upscaler right now (r.NGX.DLSS.Enable is 0), "
		     "so this preset changes nothing until you switch to DLSS. Holding it anyway so it is "
		     "correct the moment you do."));

	if (HeldPreset == Want)
	{
		return;   // already holding it; a re-assert after an Apply is the common case and is not news
	}

	const FString Value = FString::FromInt(Want);
	const bool bHeld = FPMCVarWriter::Get().Hold(
		GFPMPresetOwner, GFPMPresetCVar, *Value,
		TEXT("the game requests Preset C, the old CNN model that ghosts on movers; the player asked for "
		     "a transformer preset instead"));

	if (bHeld)
	{
		HeldPreset = Want;
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] upscaler preset: holding %s = %d (%s) at %s. The game asked for %d."),
			GFPMPresetCVar, Want,
			Want == 10 ? TEXT("preset J") : (Want == 11 ? TEXT("preset K") : TEXT("a game-level preset")),
			Moment, GFPMPresetGameRequested);
	}
	// A refusal is already logged with its reason by the writer itself - do not double-report it.
}

void FFPMUpscalerPreset::ReportNow()
{
	const int32 Live = ReadLiveCVar(GFPMPresetCVar);
	const int32 Want = CVarDLSSPreset.GetValueOnGameThread();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] upscaler preset: %s"), *FPMUpscaler::Describe());

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %s live value = %d | game originally asked for %d | FPM.Upscaler.DLSSPreset = %d"),
		GFPMPresetCVar, Live, GFPMPresetGameRequested, Want);

	/*
	 * ⚠ THE ONE COMPARISON THAT MATTERS, AND IT IS NOT "did the write succeed". If the live value does
	 * not match what we hold, something outranked us - which for this cvar means FG rewrote it, and that
	 * is R4's hazard occurring rather than being prevented. Say it as the finding it is.
	 */
	UE_CLOG(Want != 0 && Live != Want, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   MISMATCH: we asked for %d and the live value is %d. Something outranked the hold - "
		     "for this cvar that most likely means FactoryGame's own upscaler code rewrote it after our "
		     "re-assert. That is the ownership hazard, observed rather than assumed."),
		Want, Live);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ⚠ A preset does NOT fix particles smearing over surfaces. That is the missing "
		     "reactive mask - the game ships r.FidelityFX.FSR3.CreateReactiveMask=False and authored no "
		     "DLSS stencil value, so no upscaler is told which pixels are reactive. Separate work."));
}

static FAutoConsoleCommand GFPMUpscalerReportCmd(
	TEXT("FPM.Upscaler.Report"),
	TEXT("Which upscaler is live, the DLSS preset the game asked for, and the one FPM is holding."),
	FConsoleCommandDelegate::CreateStatic(&FFPMUpscalerPreset::ReportNow));
