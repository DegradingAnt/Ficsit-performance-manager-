// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMNaniteStreamingGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Rendering/NaniteStreamingManager.h"

namespace
{
	/**
	 * The ENGINE's default (`NaniteStreamingManager.cpp:134`). Kept only as the fallback when the cvar
	 * cannot be read at all — it is NOT what this game runs. See `ReservedMB` and the header.
	 */
	constexpr int32 GEngineDefaultPoolMB = 512;

	const TCHAR* const GPoolCVar = TEXT("r.Nanite.Streaming.StreamingPoolSize");

	TAutoConsoleVariable<float> CVarNaniteSampleSeconds(
		TEXT("FPM.Nanite.SampleSeconds"), 1.0f,
		TEXT("How often to read Nanite's live quality-scale factor. One float read, no allocation - the "
		     "cost is the ticker itself. A dip shorter than this interval will not be seen, which is why "
		     "the report says 'never caught below 1.0' rather than 'never scaled'."),
		ECVF_Default);

	FTSTicker::FDelegateHandle GNaniteTicker;

	float GLastFactor = 1.0f;
	float GMinFactor = 1.0f;
	double GSecondsScaledDown = 0.0;
	int32 GNaniteSamples = 0;
	int32 GSamplesScaledDown = 0;

	/**
	 * ⚠ UNSYNCHRONISED BY DESIGN. See the header. An aligned four-byte load cannot tear, and this value
	 * steers nothing — it only decides what to print.
	 */
	float ReadQualityScale()
	{
		return Nanite::GStreamingManager.GetQualityScaleFactor();
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
	 * ★ READ THE LIVE CVAR. On this game it is 50, not the engine's 512 — CSS sets
	 * `r.Nanite.Streaming.StreamingPoolSize=50` in their cooked `FactoryGame/Config/DefaultEngine.ini:47`.
	 * The texture pool guard needs the number Nanite is ACTUALLY configured with, and a private copy of
	 * the engine constant would have been wrong by 10x on every machine running this game.
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
		FTickerDelegate::CreateLambda([](float Delta)
		{
			const float Factor = ReadQualityScale();
			GLastFactor = Factor;
			++GNaniteSamples;
			GMinFactor = FMath::Min(GMinFactor, Factor);

			if (Factor < 1.0f)
			{
				++GSamplesScaledDown;
				GSecondsScaledDown += Delta;
			}

			return true;
		}), Interval);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] nanite streaming meter ARMED, sampling every %.1f s. Pool is %d MB. It WATCHES "
		     "Nanite's quality-scale factor and writes nothing - the raise it used to carry was removed "
		     "on 2026-08-10 after 1101 samples in Ant's own base never caught the factor below 1.00. "
		     "FPM.Nanite.Report."),
		Interval, ReservedMB());
}

void FFPMNaniteStreamingGuard::Disarm()
{
	LogReport();

	if (GNaniteTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GNaniteTicker);
		GNaniteTicker.Reset();
	}
}

void FFPMNaniteStreamingGuard::LogReport(FOutputDevice* Ar)
{
	const int32 PoolMB = ReservedMB();

	TArray<FString> Lines;

	Lines.Add(FString::Printf(
		TEXT("[FPM] nanite: pool %d MB, live quality scale %.2f, minimum seen %.2f. %d sample(s), %d of "
		     "them scaled down, %.1f s below 1.0."),
		PoolMB, GLastFactor, GMinFactor, GNaniteSamples, GSamplesScaledDown, GSecondsScaledDown));

	/*
	 * ★ THE VERDICT PRINTS THE LIVE POOL, NOT A CONSTANT.
	 *
	 * The version of this line shipped in 0.11.0 said "does not overcommit a 512 MB pool" one line under
	 * a correct "pool 50 MB" — it interpolated `GEngineDefaultPoolMB` instead of the measured value, so
	 * the readout contradicted itself in adjacent sentences. Ant read both lines together and caught it.
	 * A diagnostic that disagrees with its own first line is worse than one that says nothing.
	 */
	if (GNaniteSamples == 0)
	{
		Lines.Add(TEXT("[FPM]   NO SAMPLES. The ticker never fired, so nothing here has been measured - "
		               "this is a dead readout, not a clean one."));
	}
	else if (GMinFactor >= 1.0f)
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   never caught below 1.0 in %d sample(s). Either this scene does not overcommit a "
			     "%d MB pool, or every dip was shorter than the %.1f s sample interval. A real negative "
			     "result - Nanite is NOT dropping geometric detail for pool pressure here."),
			GNaniteSamples, PoolMB, FMath::Max(0.25f, CVarNaniteSampleSeconds.GetValueOnGameThread())));
	}
	else
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM]   ⚠ CAUGHT IT: the factor reached %.2f (floor is QualityScale.MinQuality 0.30), so "
			     "geometry WAS dropping detail because the %d MB pool went past "
			     "QualityScale.MaxPoolPercentage 85%%. This contradicts the 2026-08-10 measurement that "
			     "justified removing the raise - reopen that decision with these numbers."),
			GMinFactor, PoolMB));
	}

	for (const FString& L : Lines)
	{
		if (Ar != nullptr) { Ar->Log(L); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *L);
	}
}

static FAutoConsoleCommandWithOutputDevice GNaniteReportCmd(
	TEXT("FPM.Nanite.Report"),
	TEXT("Print Nanite's live quality-scale factor and whether geometry has been dropping detail because "
	     "the streaming pool is overcommitted."),
	/*
	 * ⚠ NO FPMScopedConsoleEcho HERE, and that is not an omission. LogReport already takes the device and
	 * writes to it directly. Adding the echo as well would mirror GLog into the same device and print
	 * every line TWICE. The echo is for the reports that CANNOT take a device without a large rewrite —
	 * PSO, stall, blueprint. Two mechanisms, one per shape, and never both on one command.
	 */
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		// The one-per-frame cap. This report's own listing is a fixed set of lines, so the gate is here
		// for the CALL RATE and nothing else: it is the driver that was expensive on 2026-08-15, not
		// the body. See FPMConsoleEcho.h.
		FPMReportGate Gate(Ar, TEXT("FPM.Nanite.Report"));
		if (Gate.IsRefused())
		{
			return;
		}

		FFPMNaniteStreamingGuard::LogReport(&Ar);
	}));
