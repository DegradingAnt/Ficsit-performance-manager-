// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMNaniteStreamingGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"
#include "Fixes/Interop/FPMTexturePoolGuard.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/NaniteStreamingManager.h"

namespace
{
	/** The engine's own default, `NaniteStreamingManager.cpp:134`. The number FPM must beat to matter. */
	constexpr int32 GEngineDefaultPoolMB = 512;

	const TCHAR* const GPoolCVar = TEXT("r.Nanite.Streaming.StreamingPoolSize");
	const FName GNaniteOwner(TEXT("nanite-streaming-guard"));

	TAutoConsoleVariable<int32> CVarNanitePoolMB(
		TEXT("FPM.Nanite.PoolMB"), 0,
		TEXT("Target size in MB for Nanite's streaming pool, applied only once quality scaling is actually "
		     "observed. 0 = pick from the card's VRAM. The engine's own default is 512, and going over it "
		     "is what stops geometry dropping to low detail in a dense base. -1 disables the raise and "
		     "leaves this fix as a pure meter."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarNaniteSampleSeconds(
		TEXT("FPM.Nanite.SampleSeconds"), 1.0f,
		TEXT("How often to read Nanite's live quality-scale factor. One float read, no allocation - the "
		     "cost is the ticker itself. A dip shorter than this interval will not be seen, which is why "
		     "the report says 'never caught below 1.0' rather than 'never scaled'."),
		ECVF_Default);

	FTSTicker::FDelegateHandle GNaniteTicker;

	float GLastFactor = 1.0f;
	float GMinFactorBeforeRaise = 1.0f;
	float GMinFactorAfterRaise = 1.0f;
	double GSecondsScaledDown = 0.0;
	int32 GNaniteSamples = 0;
	int32 GSamplesScaledDown = 0;
	bool  GNaniteRaised = false;
	int32 GRaisedToMB = 0;
	int32 GSamplesAfterRaise = 0;

	/**
	 * ⚠ UNSYNCHRONISED BY DESIGN. See the header. An aligned four-byte load cannot tear, and this value
	 * steers nothing — it only decides whether to raise the pool once and what to print.
	 */
	float ReadQualityScale()
	{
		return Nanite::GStreamingManager.GetQualityScaleFactor();
	}

	/**
	 * ★ HOW BIG TO MAKE IT, AND WHY NOT SIMPLY "AS BIG AS POSSIBLE".
	 *
	 * The engine's own warning on this cvar (`NaniteStreamingManager.cpp:141-143`): *"Be careful with
	 * setting this close to the GPU resource size limit (typically 2-4GB) as root pages are allocated
	 * from the same physical buffer."* So the ceiling is not the card, it is the buffer, and overshooting
	 * it trades a detail drop for an allocation failure.
	 *
	 * One eighth of the card, clamped to [512, 2048]:
	 *  - the floor is the engine default, so this can never make things WORSE than vanilla;
	 *  - the cap keeps well clear of the 2-4 GB buffer limit the engine warns about;
	 *  - one eighth gives 2 GB on a 16 GB card and 1 GB on an 8 GB one, which scales the way the texture
	 *    pool guard's own fractions already do rather than inventing a second philosophy.
	 *
	 * ⚠ THESE ARE REASONED BOUNDS, NOT MEASURED ONES. The measurement that would refine them is the one
	 * this fix produces: if the factor still drops after the raise, the cap is too low for her base and
	 * the report will say so.
	 */
	int32 ChooseTargetMB()
	{
		const int32 Configured = CVarNanitePoolMB.GetValueOnGameThread();
		if (Configured > 0) { return Configured; }

		const int64 VramMB = FPMTexturePool::QueryTotalVramMB();
		if (VramMB <= 0) { return 0; }   // the RHI has not answered yet; stand down rather than guess

		return static_cast<int32>(FMath::Clamp<int64>(VramMB / 8, GEngineDefaultPoolMB, 2048));
	}

	void RaiseOnce()
	{
		if (GNaniteRaised) { return; }
		if (CVarNanitePoolMB.GetValueOnGameThread() < 0) { return; }   // meter-only mode

		const int32 TargetMB = ChooseTargetMB();
		if (TargetMB <= GEngineDefaultPoolMB)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] nanite: quality scaling seen (%.2f) but the computed pool target is %d MB, "
				     "which is not above the engine's own %d. Standing down rather than writing a value "
				     "that changes nothing - the card is too small, or the RHI has not reported VRAM."),
				GLastFactor, TargetMB, GEngineDefaultPoolMB);
			GNaniteRaised = true;   // latched: do not re-derive the same answer every sample
			return;
		}

		const bool bHeld = FPMCVarWriter::Get().Hold(
			GNaniteOwner, GPoolCVar, *FString::FromInt(TargetMB),
			TEXT("nanite streaming: pool overcommitted, geometry was dropping to low detail"));

		if (!bHeld)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] nanite: the writer REFUSED '%s'. Its reason is the line above this one. The "
				     "meter keeps running; the pool stays at the engine's value."), GPoolCVar);
			GNaniteRaised = true;
			return;
		}

		GNaniteRaised = true;
		GRaisedToMB = TargetMB;
		GMinFactorAfterRaise = 1.0f;
		GSamplesAfterRaise = 0;

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] nanite: quality scaling caught at %.2f (1.0 = none, floor is "
			     "QualityScale.MinQuality 0.30). Nanite's streaming pool was overcommitted past "
			     "QualityScale.MaxPoolPercentage 85%%, so geometry near you was dropping detail. Pool "
			     "%d -> %d MB. The texture pool guard's reservation follows this number automatically. "
			     "FPM.Nanite.Report says whether it worked."),
			GLastFactor, GEngineDefaultPoolMB, TargetMB);

		FPMOverlay::Post(TEXT("nanite"),
			FString::Printf(TEXT("pool %d MB (was %d), scale had dropped to %.2f"),
				TargetMB, GEngineDefaultPoolMB, GMinFactorBeforeRaise));
	}
}

FFPMNaniteStreamingGuard& FFPMNaniteStreamingGuard::Get()
{
	static FFPMNaniteStreamingGuard Instance;
	return Instance;
}

int32 FFPMNaniteStreamingGuard::ReservedMB()
{
	/*
	 * Read the LIVE cvar rather than returning GRaisedToMB. Another mod, a config, or Ant's own console
	 * could have set it, and the texture pool guard needs to know how much VRAM Nanite is ACTUALLY
	 * taking, not how much FPM asked for. Falls back to the engine default when the cvar is missing,
	 * which is the same number that constant used to be.
	 */
	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(GPoolCVar))
	{
		const int32 Live = Var->GetInt();
		if (Live > 0) { return Live; }
	}
	return GEngineDefaultPoolMB;
}

void FFPMNaniteStreamingGuard::Arm()
{
	if (GNaniteTicker.IsValid()) { return; }

	const float Interval = FMath::Max(0.25f, CVarNaniteSampleSeconds.GetValueOnGameThread());

	GNaniteTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Interval](float Delta)
		{
			const float Factor = ReadQualityScale();
			GLastFactor = Factor;
			++GNaniteSamples;

			if (Factor < 1.0f)
			{
				++GSamplesScaledDown;
				GSecondsScaledDown += Delta;
			}

			if (GNaniteRaised)
			{
				++GSamplesAfterRaise;
				GMinFactorAfterRaise = FMath::Min(GMinFactorAfterRaise, Factor);
			}
			else
			{
				GMinFactorBeforeRaise = FMath::Min(GMinFactorBeforeRaise, Factor);
				if (Factor < 1.0f) { RaiseOnce(); }
			}

			return true;
		}), Interval);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] nanite streaming guard ARMED, sampling every %.1f s. It reads Nanite's live "
		     "quality-scale factor and does NOTHING until that factor actually drops below 1.0 - a "
		     "machine whose pool never overcommits never gets a write. When it does drop, the pool goes "
		     "above the engine's %d MB and the meter keeps running so the report can say whether that "
		     "helped. FPM.Nanite.Report."),
		Interval, GEngineDefaultPoolMB);
}

void FFPMNaniteStreamingGuard::Disarm()
{
	LogReport();

	if (GNaniteTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GNaniteTicker);
		GNaniteTicker.Reset();
	}

	FPMCVarWriter::Get().Release(GNaniteOwner, GPoolCVar);
}

void FFPMNaniteStreamingGuard::LogReport(FOutputDevice* Ar)
{
	TArray<FString> Lines;

	Lines.Add(FString::Printf(
		TEXT("[FPM] nanite: pool %d MB (engine default %d), live quality scale %.2f. %d sample(s), %d of "
		     "them scaled down, %.1f s below 1.0."),
		ReservedMB(), GEngineDefaultPoolMB, GLastFactor, GNaniteSamples, GSamplesScaledDown,
		GSecondsScaledDown));

	/*
	 * ★ THE VERDICT, AND IT WILL NOT CALL AN UNRUN TEST A PASS.
	 *
	 * Zero samples means the ticker never fired. "Min 1.0 over N samples" is a real negative result and
	 * is stated as one — it means her save does not reproduce the overcommit at these sample points,
	 * which is worth knowing and is NOT the same as the fix working.
	 */
	if (GNaniteSamples == 0)
	{
		Lines.Add(TEXT("[FPM]   NO SAMPLES. The ticker never fired, so nothing here has been measured - "
		               "this is a dead readout, not a clean one."));
	}
	else if (!GNaniteRaised)
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   never caught below 1.0 in %d sample(s), so the pool was never raised. Either "
			     "this scene does not overcommit a %d MB pool, or every dip was shorter than the %.1f s "
			     "sample interval. A negative result, not a fix."),
			GNaniteSamples, GEngineDefaultPoolMB, FMath::Max(0.25f, CVarNaniteSampleSeconds.GetValueOnGameThread())));
	}
	else if (GRaisedToMB <= 0)
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   scaling was seen (min %.2f) but the raise did NOT happen - the reason was "
			     "logged when it was decided. The pool is still the engine's."), GMinFactorBeforeRaise));
	}
	else if (GSamplesAfterRaise == 0)
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   pool raised to %d MB after catching %.2f, but NO samples have been taken since. "
			     "Play for a moment and run this again - there is no after-measurement yet."),
			GRaisedToMB, GMinFactorBeforeRaise));
	}
	else if (GMinFactorAfterRaise >= 1.0f)
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   IT WORKED. Min was %.2f before the raise and has stayed at 1.00 across %d "
			     "sample(s) since %d MB. Geometry is no longer being dropped for pool pressure."),
			GMinFactorBeforeRaise, GSamplesAfterRaise, GRaisedToMB));
	}
	else
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   NOT ENOUGH. Min %.2f before the raise, still %.2f across %d sample(s) at %d MB. "
			     "The pool is bigger and the scene still overcommits it - raise FPM.Nanite.PoolMB by hand "
			     "and re-measure, and mind the engine's warning about the 2-4 GB buffer limit."),
			GMinFactorBeforeRaise, GMinFactorAfterRaise, GSamplesAfterRaise, GRaisedToMB));
	}

	for (const FString& L : Lines)
	{
		if (Ar != nullptr) { Ar->Log(L); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *L);
	}
}

/*
 * Takes the output device so the answer lands in the console she typed it into as well as the log — a
 * Display-level UE_LOG alone does not echo there, and a command that answers somewhere the operator is
 * not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GNaniteReportCmd(
	TEXT("FPM.Nanite.Report"),
	TEXT("Print Nanite's live quality-scale factor, whether geometry has been dropping detail for pool "
	     "pressure, and whether raising the streaming pool fixed it."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMNaniteStreamingGuard::LogReport(&Ar);
	}));
