// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMUpscalerSharpness.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"

#include "HAL/IConsoleManager.h"

namespace
{
	const FName GFPMSharpOwner(TEXT("upscaler-sharpness"));

	/** Verified from the shipped FSR plugin DLL, 2026-08-11. FSR's own RCAS pass. */
	const TCHAR* GFPMFSRSharpCVar = TEXT("r.FidelityFX.FSR.Sharpness");

	/** Verified from PostProcessTonemap.cpp:36-41. Engine-side, always present. */
	const TCHAR* GFPMTonemapSharpCVar = TEXT("r.Tonemapper.Sharpen");

	/*
	 * ⚠ THE OFF VALUE IS -1, NOT 0, AND THAT IS THE ENGINE'S OWN SEMANTICS RATHER THAN A PREFERENCE.
	 * PostProcessTonemap.cpp:38-41 - default -1, "<0: inherit from PostProcessVolume settings
	 * (default) / 0: off". Releasing to 0 would TURN OFF a sharpen the level asked for; releasing to
	 * -1 hands the decision back. The writer's Release restores the prior value anyway, so this
	 * constant exists for the "player set 0" case, which means hands-off, not off.
	 */
	constexpr float GFPMSharpHandsOff = 0.f;

	TAutoConsoleVariable<float> CVarSharpAmount(
		TEXT("FPM.Sharpness.Amount"), 0.f,
		TEXT("Sharpening strength, 0 = leave the game alone (default). The lever used depends on which "
		     "upscaler is live: FSR gets its own RCAS sharpener (0..1), TSR and native get the "
		     "tonemapper's sharpen (0..2). DLSS and XeSS get NOTHING - they sharpen internally and "
		     "adding more on top produces ringing on every edge."),
		ECVF_Default);

	float Clamped(EFPMUpscaler For, float Raw)
	{
		// The two levers have different useful ranges; FPM1 shipped 0..1 for FSR and 0..2 for the
		// tonemapper, and nothing since has contradicted those bounds.
		return (For == EFPMUpscaler::FSR) ? FMath::Clamp(Raw, 0.f, 1.f) : FMath::Clamp(Raw, 0.f, 2.f);
	}
}

FFPMUpscalerSharpness& FFPMUpscalerSharpness::Get()
{
	static FFPMUpscalerSharpness Instance;
	return Instance;
}

void FFPMUpscalerSharpness::Arm()
{
	CVarSharpAmount.AsVariable()->SetOnChangedCallback(
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
		{
			FFPMUpscalerSharpness::Get().Route(TEXT("cvar changed"));
		}));

	// Ungated by the diag channel: the stated Arm()-line exception.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] upscaler sharpness ARMED - no hook. FPM.Sharpness.Amount routes to the lever that "
		     "matches the LIVE upscaler: FSR gets r.FidelityFX.FSR.Sharpness, TSR and native get "
		     "r.Tonemapper.Sharpen, and DLSS/XeSS get nothing because they sharpen internally. The two "
		     "must never both apply - stacking them rings every edge, which is why FPM1 shipped this as "
		     "two knobs with 'never stacked on DLSS/XeSS/FSR' written on one of them."));
}

void FFPMUpscalerSharpness::Disarm()
{
	FPMCVarWriter::Get().ReleaseOwner(GFPMSharpOwner);
	RoutedFor = EFPMUpscaler::Unknown;
}

void FFPMUpscalerSharpness::OnWorldLoad(UWorld* /*World*/)
{
	/*
	 * Re-route on every load. The player can change upscaler in the options between sessions, and a
	 * sharpener left on the lever for the PREVIOUS upscaler is both wrong and invisible - the value
	 * sits there doing nothing while the report claims a hold.
	 */
	Route(TEXT("world load"));
}

void FFPMUpscalerSharpness::Route(const TCHAR* Moment)
{
	const float Want = CVarSharpAmount.GetValueOnGameThread();
	const EFPMUpscaler Live = FPMUpscaler::Current();

	/*
	 * ★ RELEASE FIRST, ALWAYS. Whatever we held may belong to a lever that no longer applies - the
	 * player switched from FSR to DLSS, or asked for 0. Releasing unconditionally before deciding what
	 * to hold means the two sharpeners cannot both be live even for a frame, which is the one outcome
	 * this fix exists to prevent.
	 */
	FPMCVarWriter::Get().ReleaseOwner(GFPMSharpOwner);

	if (Want <= GFPMSharpHandsOff)
	{
		UE_CLOG(RoutedFor != EFPMUpscaler::Unknown && FPMDiag::IsOn(FPMDiag::EChannel::Sharpness),
			LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] sharpness: released at %s - FPM.Sharpness.Amount is 0, the game's own sharpening "
			     "is back in charge."), Moment);
		RoutedFor = EFPMUpscaler::Unknown;
		return;
	}

	/*
	 * ⚠ DLSS AND XeSS GET NOTHING, AND THIS IS A REFUSAL RATHER THAN AN OMISSION. Both sharpen inside
	 * their own reconstruction. There is no second lever we could add that would not be additive on
	 * top of it, so the honest answer to "sharpen my DLSS image" is "not from here" - said out loud,
	 * because a silent no-op would leave the player turning a knob that does nothing.
	 */
	if (Live == EFPMUpscaler::DLSS || Live == EFPMUpscaler::XeSS)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] sharpness: %s is live, so FPM.Sharpness.Amount=%.2f is IGNORED. %s sharpens "
			     "inside its own upscaling pass and anything added on top rings the edges. For DLSS, the "
			     "preset is the lever that changes image quality - see FPM.Upscaler.DLSSPreset."),
			FPMUpscaler::NameOf(Live), Want, FPMUpscaler::NameOf(Live));
		RoutedFor = Live;
		return;
	}

	/*
	 * ⚠ UNKNOWN IS NOT A LICENCE TO GUESS. If the probe could not determine the upscaler, applying the
	 * tonemapper sharpen would be a coin flip that lands on "stacked on DLSS" half the time. Refuse and
	 * say why - the probe's own Describe() prints the inputs so this is diagnosable rather than mute.
	 */
	if (Live == EFPMUpscaler::Unknown)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] sharpness: the live upscaler could not be determined, so nothing was applied - "
			     "guessing here risks stacking sharpening on top of DLSS. %s"),
			*FPMUpscaler::Describe());
		RoutedFor = EFPMUpscaler::Unknown;
		return;
	}

	const bool bFSR = (Live == EFPMUpscaler::FSR);
	const TCHAR* CVarName = bFSR ? GFPMFSRSharpCVar : GFPMTonemapSharpCVar;
	const float Value = Clamped(Live, Want);

	const FString ValueStr = FString::SanitizeFloat(Value);
	const bool bHeld = FPMCVarWriter::Get().Hold(
		GFPMSharpOwner, CVarName, *ValueStr,
		TEXT("the player asked for sharpening and this is the lever that matches the live upscaler"));

	if (bHeld)
	{
		RoutedFor = Live;
		UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Sharpness), LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] sharpness: %s is live, holding %s = %s at %s."),
			FPMUpscaler::NameOf(Live), CVarName, *ValueStr, Moment);
	}
	// A refusal is logged with its reason by the writer itself; do not double-report it.
}

void FFPMUpscalerSharpness::ReportNow()
{
	const FFPMUpscalerSharpness& Self = Get();
	const EFPMUpscaler Live = FPMUpscaler::Current();
	const float Want = CVarSharpAmount.GetValueOnGameThread();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] sharpness: requested %.2f | %s"), Want, *FPMUpscaler::Describe());

	const TCHAR* Lever =
		(Live == EFPMUpscaler::FSR)                                     ? GFPMFSRSharpCVar :
		(Live == EFPMUpscaler::DLSS || Live == EFPMUpscaler::XeSS)      ? TEXT("(none - it sharpens itself)") :
		(Live == EFPMUpscaler::Unknown)                                 ? TEXT("(none - upscaler unknown)") :
		                                                                  GFPMTonemapSharpCVar;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   lever for this upscaler: %s | routed for: %s"),
		Lever, FPMUpscaler::NameOf(Self.RoutedFor));

	/*
	 * ⚠ SAY WHEN THE FSR PATH IS THE LEAST TESTED ONE. Board m5664350 records FSR shipping with green
	 * chroma corruption on this stack, so a sharpness value on that path may be sitting on top of a
	 * broken image. Better said than discovered.
	 */
	UE_CLOG(Live == EFPMUpscaler::FSR, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   NOTE: FSR is the least-exercised path here - board m5664350 records it shipping "
		     "with green chroma corruption on this hardware. Judge sharpness after that, not before."));
}

static FAutoConsoleCommand GFPMSharpReportCmd(
	TEXT("FPM.Sharpness.Report"),
	TEXT("Sharpening: the live upscaler, which lever that selects, and what is currently held."),
	FConsoleCommandDelegate::CreateStatic(&FFPMUpscalerSharpness::ReportNow));
