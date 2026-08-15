// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMSteerSignal.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"

namespace
{
	/** The user's target frame rate, section 3.5's first surviving user bound.
	 *
	 *  ⚠ IT IS OUR OWN CVAR, not a US_*-backed one, which is what makes it zero-residue: registered by
	 *  this module, gone when the module unloads, never serialised into GameUserSettings.ini. The
	 *  settings ROW that will drive it is section 6 work; the cvar has a consumer today, which is the
	 *  order this project ships things in (see FPMMasterSwitch.h's own note on the same choice). */
	TAutoConsoleVariable<float> GSteerTargetFPS(
		TEXT("FPM.Governor.TargetFPS"),
		60.0f,
		TEXT("The frame rate the governor steers to. BudgetMs and RaiseMs are derived from it every "
		     "tick, so changing it takes effect immediately and cannot leave a stale partner value."),
		ECVF_Default);

	/** Bounds on the target, applied at the derivation rather than trusted from the caller. A zero
	 *  would divide; a 500 would make every budget unreachable and the ladder would cut for ever. */
	constexpr float GTargetFPSMin = 20.0f;
	constexpr float GTargetFPSMax = 300.0f;

	/** t.MaxFPS, READ ONLY. Section 3.9 lists it among the things the governor never touches: it is
	 *  US_*-backed, so a hold would become the player's permanent setting. FPM recommends a cap in
	 *  the UI (section 3.10) and never writes one. */
	float SteerReadMaxFPS()
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"), false);
		return Var ? Var->GetFloat() : 0.0f;
	}
}

float FPMBudgetSpreadFPS()
{
	return 15.0f;
}

FFPMSteerBudgets FPMDeriveBudgets(const float TargetFPS, const float MaxFPS)
{
	FFPMSteerBudgets Out;
	Out.TargetFPS = FMath::Clamp(TargetFPS, GTargetFPSMin, GTargetFPSMax);
	Out.MaxFPS = MaxFPS;

	if (MaxFPS > 0.0f)
	{
		// Under a frame cap the target is not the thing being missed -- the cap is. Carried from the
		// FPM1 archive at :4104-4107: the two multipliers keep a small dead-band around the cap
		// instead of a spread that a cap makes meaningless.
		const float CapMs = 1000.0f / FMath::Max(MaxFPS, 1.0f);
		Out.BudgetMs = CapMs * 1.06f;
		Out.RaiseMs  = CapMs * 1.02f;
		Out.bFromFrameCap = true;
		Out.Derivation = FString::Printf(
			TEXT("frame cap %.0f fps -> %.3f ms/frame; budget = cap*1.06, raise = cap*1.02. "
			     "t.MaxFPS is READ here and never written (section 3.9)."),
			MaxFPS, CapMs);
		return Out;
	}

	Out.BudgetMs = 1000.0f / Out.TargetFPS;
	Out.RaiseMs  = 1000.0f / (Out.TargetFPS + FPMBudgetSpreadFPS());
	Out.bFromFrameCap = false;
	Out.Derivation = FString::Printf(
		TEXT("uncapped: budget = 1000/%.0f, raise = 1000/(%.0f+%.0f). The +%.0f spread IS the "
		     "dead-band (Normal utilisation mode, section 3.11)."),
		Out.TargetFPS, Out.TargetFPS, FPMBudgetSpreadFPS(), FPMBudgetSpreadFPS());
	return Out;
}

float FPMUpdateEma(const float Current, const float SampleMs, const float DeltaSeconds,
                   const float TauSeconds)
{
	if (Current < 0.0f)
	{
		// Unprimed. Priming from the first sample rather than from zero, because a mean that starts at
		// zero reads as a machine hitting 0 ms and would ask the ladder to promote on its first tick.
		return SampleMs;
	}
	if (DeltaSeconds <= 0.0f || TauSeconds <= 0.0f)
	{
		return Current;
	}

	// alpha = 1 - exp(-dt/tau). At dt << tau this is nearly dt/tau, and at dt >> tau it saturates at
	// 1, which is the correct behaviour after a long stall: the mean adopts the new sample rather
	// than crawling toward it through a window that has already ended.
	const float Alpha = 1.0f - FMath::Exp(-DeltaSeconds / TauSeconds);
	return Current + Alpha * (SampleMs - Current);
}

float FPMLowPercentileMs(const TArray<float>& SamplesMs, const float Percent)
{
	if (Percent <= 0.0f || Percent >= 100.0f)
	{
		return -1.0f;
	}

	// A "1% low" over fewer than 100 samples is one sample wearing a statistic's name. Refusing is the
	// honest answer and the caller prints it.
	const int32 Needed = FMath::CeilToInt(100.0f / Percent);
	if (SamplesMs.Num() < Needed)
	{
		return -1.0f;
	}

	TArray<float> Sorted = SamplesMs;
	Sorted.Sort();

	// The slow tail: the 1% low is the frame time at the 99th percentile of frame TIMES.
	const int32 Index = FMath::Clamp(
		FMath::FloorToInt(static_cast<float>(Sorted.Num()) * (1.0f - Percent / 100.0f)),
		0, Sorted.Num() - 1);
	return Sorted[Index];
}

// ------------------------------------------------------------------------------------------------

FFPMSteerSignal& FFPMSteerSignal::Get()
{
	static FFPMSteerSignal Instance;
	return Instance;
}

bool FFPMSteerSignal::Tick(float /*SmoothedEngineDeltaDoNotUse*/)
{
	// ★ THE FRAME CLOCK, AND NOT THE TICKER'S OWN ARGUMENT. The parameter is the engine's SMOOTHED
	// delta; smoothing a signal we are about to smooth ourselves makes the time constant a fiction,
	// and the hitch meter names the same parameter the same way for the same reason.
	const double Now = FPlatformTime::Seconds();
	if (LastSampleSeconds <= 0.0)
	{
		LastSampleSeconds = Now;
		return true;
	}

	const double DeltaSeconds = Now - LastSampleSeconds;
	LastSampleSeconds = Now;
	if (DeltaSeconds <= 0.0)
	{
		return true;
	}

	const float SampleMs = static_cast<float>(DeltaSeconds * 1000.0);
	MeanMs = FPMUpdateEma(MeanMs, SampleMs, static_cast<float>(DeltaSeconds), MeanTauSeconds());
	WorstSampleMs = FMath::Max(WorstSampleMs, SampleMs);
	++SamplesSeen;

	// The low window, as a ring so a long session costs a fixed amount of memory.
	if (LowWindow.Num() < LowWindowSamples())
	{
		LowWindow.Add(SampleMs);
	}
	else
	{
		LowWindow[LowWindowNext] = SampleMs;
		LowWindowNext = (LowWindowNext + 1) % LowWindow.Num();
	}

	// Derived every tick from the live values, so there is no partner value that can go stale
	// (board m6138429). The copy kept here is for the REPORT, never for a decision.
	LastBudgets = FPMDeriveBudgets(GSteerTargetFPS.GetValueOnGameThread(), SteerReadMaxFPS());

	return true;
}

bool FFPMSteerSignal::BuildInputs(const EFPMGovernorMode Mode, FFPMSteeringInputs& Out,
                                  FString& OutCoverage) const
{
	Out = FFPMSteeringInputs();
	Out.Mode = Mode;
	Out.NowSeconds = FPlatformTime::Seconds();
	Out.MeanFrameMs = FMath::Max(MeanMs, 0.0f);

	const FFPMSteerBudgets Budgets = FPMDeriveBudgets(GSteerTargetFPS.GetValueOnGameThread(),
	                                                  SteerReadMaxFPS());
	Out.BudgetMs = Budgets.BudgetMs;
	Out.RaiseMs  = Budgets.RaiseMs;

	// ⚠ THE THREE THINGS LEFT AT THE VALUE THAT MAKES THE WALK REFUSE. Each is left there because
	// this file has no channel to the answer, and a guess would be a claim.
	Out.Attribution = EFPMBoundAttribution::Unknown;
	Out.bResolutionAtFloor = false;
	Out.bResolutionAtMax = false;
	Out.bProfileAvailable = false;
	Out.BenchNoiseFloorMs = 0.0f;

	const bool bPrimed = SamplesSeen >= MinSamplesToSteer() && MeanMs >= 0.0f;

	OutCoverage = FString::Printf(
		TEXT("mean %s ms over %d sample(s) [%s]; budget %.3f / raise %.3f (dead-band %.3f ms). "
		     "NOT SUPPLIED: bind attribution stays Unknown (the hard-drop binder, section 3.6, is not "
		     "built -- so every cut is refused as AttributionUnknown, which is the honest block); "
		     "the resolution position stays at 'not at floor, not at max' (section 8 owns the "
		     "resolution lever and has no executor, so neither end can be claimed); "
		     "bProfileAvailable stays false (no bench, section 4 -- section 3.5a then "
		     "excludes every stage tier). Steer on this: %s."),
		MeanMs < 0.0f ? TEXT("<unprimed>") : *FString::Printf(TEXT("%.3f"), MeanMs),
		SamplesSeen,
		bPrimed ? TEXT("primed") : TEXT("PRIMING"),
		Budgets.BudgetMs, Budgets.RaiseMs, Budgets.DeadBandMs(),
		bPrimed ? TEXT("yes") : TEXT("NO"));

	return bPrimed;
}

// ------------------------------------------------------------------------------------------------
// The liveness proof
// ------------------------------------------------------------------------------------------------

bool FFPMSteerSignal::SelfTest()
{
	bool bOk = true;
	auto Fail = [&bOk](const FString& Line)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error, TEXT("[FPM] steer signal self-test FAILED: %s"),
			*Line);
		bOk = false;
	};

	// ---- (1) THE EMA MOVES, AND CONVERGES. ---------------------------------------------------------
	{
		// ⚠ IT PRIMES AT 30 AND CONVERGES TO 16, NOT THE OTHER WAY ROUND. An earlier draft of this
		// check primed from the same 16 ms it then fed, so the mean was correct after ONE call and the
		// convergence was never exercised: `return Current` would have passed it. A convergence test
		// whose starting point is the answer is a constant with extra steps.
		float Mean = FPMUpdateEma(-1.0f, 30.0f, 1.0f / 60.0f, MeanTauSeconds());
		if (!FMath::IsNearlyEqual(Mean, 30.0f, 0.001f))
		{
			Fail(FString::Printf(TEXT("(1) priming did not adopt the first sample: %.4f"), Mean));
		}
		for (int32 I = 0; I < 600; ++I)   // 10 s at 60fps = 20 tau
		{
			Mean = FPMUpdateEma(Mean, 16.0f, 1.0f / 60.0f, MeanTauSeconds());
		}
		if (!FMath::IsNearlyEqual(Mean, 16.0f, 0.01f))
		{
			Fail(FString::Printf(
				TEXT("(1) starting at 30 ms and fed a constant 16 ms for 20 time constants, the mean "
				     "settled at %.4f"), Mean));
		}

		// The mirror: a step must MOVE it. A mean that converges but cannot be moved again is a
		// constant that took a while to become one.
		const float Before = Mean;
		for (int32 I = 0; I < 30; ++I)
		{
			Mean = FPMUpdateEma(Mean, 33.0f, 1.0f / 60.0f, MeanTauSeconds());
		}
		if (Mean <= Before + 1.0f)
		{
			Fail(FString::Printf(
				TEXT("(1) a step from 16 ms to 33 ms moved the mean only %.4f -> %.4f"), Before, Mean));
		}
	}

	// ---- (2) FRAME-RATE INDEPENDENCE. --------------------------------------------------------------
	{
		// ⚠ BOTH RUNS START AT 10 ms AND ARE FED 20 ms, for the same reason check (1) starts at 30. A
		// run primed at the value it is then fed sits at that value for ever, and two constants always
		// agree -- so the earlier version of this check compared 20.0 with 20.0 and would have passed
		// against an EMA that ignored its delta entirely, which is the exact defect it exists to find.
		float Fast = FPMUpdateEma(-1.0f, 10.0f, 1.0f / 240.0f, MeanTauSeconds());
		for (int32 I = 0; I < 240; ++I)   // 240 frames at 1/240 s = 1.0 s
		{
			Fast = FPMUpdateEma(Fast, 20.0f, 1.0f / 240.0f, MeanTauSeconds());
		}
		float Slow = FPMUpdateEma(-1.0f, 10.0f, 1.0f / 30.0f, MeanTauSeconds());
		for (int32 I = 0; I < 30; ++I)    // 30 frames at 1/30 s  = 1.0 s
		{
			Slow = FPMUpdateEma(Slow, 20.0f, 1.0f / 30.0f, MeanTauSeconds());
		}

		// The mirror the comparison needs: the runs must have MOVED. Two values that agree because
		// neither one budged is not independence, it is a stuck signal.
		if (Fast <= 10.001f || Fast >= 19.999f)
		{
			Fail(FString::Printf(
				TEXT("(2) NO COVERAGE: after one second the mean sits at %.4f, so it either never left "
				     "10 ms or has already saturated at 20. Neither exercises the exponential."), Fast));
		}
		else if (!FMath::IsNearlyEqual(Fast, Slow, 0.05f))
		{
			Fail(FString::Printf(
				TEXT("(2) one second of the same signal landed at %.4f at 240fps and %.4f at 30fps. "
				     "The smoothing window depends on the frame rate, so every dwell constant means a "
				     "different thing on a different machine."), Fast, Slow));
		}
	}

	// ---- (3) THE BUDGETS DEPEND ON THE TARGET, and the arithmetic is checkable by hand. -------------
	const FFPMSteerBudgets At60 = FPMDeriveBudgets(60.0f, 0.0f);
	const FFPMSteerBudgets At30 = FPMDeriveBudgets(30.0f, 0.0f);
	if (!FMath::IsNearlyEqual(At60.BudgetMs, 16.667f, 0.01f)
	    || !FMath::IsNearlyEqual(At60.RaiseMs, 13.333f, 0.01f))
	{
		Fail(FString::Printf(
			TEXT("(3) at target 60 the budgets came out %.4f / %.4f, not 1000/60 and 1000/75"),
			At60.BudgetMs, At60.RaiseMs));
	}
	if (FMath::IsNearlyEqual(At60.BudgetMs, At30.BudgetMs, 0.01f))
	{
		Fail(TEXT("(3) targets 60 and 30 produced the same budget. The derivation does not read its "
		          "input, which is the dead-instrument shape: a confident number that never changes."));
	}

	// ---- (4) THE DEAD-BAND IS POSITIVE EVERYWHERE. -------------------------------------------------
	{
		const FFPMSteerBudgets Cases[] =
		{
			FPMDeriveBudgets(30.0f, 0.0f), FPMDeriveBudgets(60.0f, 0.0f),
			FPMDeriveBudgets(144.0f, 0.0f), FPMDeriveBudgets(60.0f, 60.0f),
			FPMDeriveBudgets(60.0f, 144.0f),
		};
		for (const FFPMSteerBudgets& B : Cases)
		{
			if (B.DeadBandMs() <= 0.0f)
			{
				Fail(FString::Printf(
					TEXT("(4) dead-band %.4f ms at target %.0f cap %.0f. Cut and boost would sit on the "
					     "same side of one number, which oscillates."),
					B.DeadBandMs(), B.TargetFPS, B.MaxFPS));
			}
		}
	}

	// ---- (5) THE CAP BRANCH IS REACHABLE AND CHANGES THE ANSWER. -----------------------------------
	{
		const FFPMSteerBudgets Capped = FPMDeriveBudgets(60.0f, 120.0f);
		if (!Capped.bFromFrameCap || FMath::IsNearlyEqual(Capped.BudgetMs, At60.BudgetMs, 0.01f))
		{
			Fail(TEXT("(5) a 120fps cap produced the same budget as the uncapped 60fps target, so the "
			          "cap branch is decoration rather than a branch."));
		}
	}

	// ---- (6) THE PERCENTILE IS A PERCENTILE. -------------------------------------------------------
	{
		TArray<float> Series;
		Series.Reserve(200);
		for (int32 I = 0; I < 198; ++I) { Series.Add(16.0f); }
		Series.Add(90.0f);
		Series.Add(95.0f);   // the slowest 1% of 200 samples is two frames

		const float Low1 = FPMLowPercentileMs(Series, 1.0f);
		if (Low1 < 89.0f)
		{
			Fail(FString::Printf(
				TEXT("(6) the 1%% low of a series whose slowest 1%% are 90 and 95 ms came back %.3f. "
				     "It is reporting the middle of the distribution, not the tail."), Low1));
		}

		TArray<float> TooShort;
		for (int32 I = 0; I < 40; ++I) { TooShort.Add(16.0f); }
		if (FPMLowPercentileMs(TooShort, 1.0f) >= 0.0f)
		{
			Fail(TEXT("(6) a 1% low was returned from 40 samples. One sample wearing a statistic's "
			          "name is exactly the number a support dump should not contain."));
		}

		// Order independence. A percentile that moves when the same numbers arrive in a different
		// order would drift with the ring buffer's wrap point and nobody would ever see why.
		TArray<float> Shuffled = Series;
		for (int32 I = Shuffled.Num() - 1; I > 0; --I)
		{
			Shuffled.Swap(I, (I * 7 + 3) % (I + 1));
		}
		if (!FMath::IsNearlyEqual(FPMLowPercentileMs(Shuffled, 1.0f), Low1, 0.001f))
		{
			Fail(TEXT("(6) the same samples in a different order gave a different 1% low."));
		}
	}

	return bOk;
}

// ------------------------------------------------------------------------------------------------
// Reporting
// ------------------------------------------------------------------------------------------------

void FFPMSteerSignal::ReportNow(FOutputDevice& Ar) const
{
	FPMReportGate Gate(Ar, TEXT("FPM.Steer.Report"));
	if (Gate.IsRefused()) { return; }
	FPMScopedConsoleEcho Echo(&Ar);

	if (!bSelfTestPassed)
	{
		Ar.Logf(TEXT("[FPM] steer signal: the self-test has NOT passed, so these numbers are not "
		             "trustworthy and this report will not dress them up. The boot log has the "
		             "failure."));
		return;
	}

	Ar.Logf(TEXT("[FPM] steering signal (section 3.6) -- measured on the frame clock, never factory "
	             "time."));

	if (MeanMs < 0.0f)
	{
		Ar.Logf(TEXT("[FPM]   mean: UNPRIMED. No frame has been sampled yet."));
	}
	else
	{
		Ar.Logf(TEXT("[FPM]   mean %.3f ms (%.1f fps) over %d sample(s); worst single frame %.1f ms."),
			MeanMs, MeanMs > 0.0f ? 1000.0f / MeanMs : 0.0f, SamplesSeen, WorstSampleMs);
	}

	Ar.Logf(TEXT("[FPM]   budget %.3f ms, raise %.3f ms, dead-band %.3f ms."),
		LastBudgets.BudgetMs, LastBudgets.RaiseMs, LastBudgets.DeadBandMs());
	Ar.Logf(TEXT("[FPM]   %s"), *LastBudgets.Derivation);

	// ★ THE LOWS ARE TELEMETRY AND THE LINE SAYS SO, because the one thing that must never happen to
	// this number is somebody steering on it (section 3.6: it was a one-way ratchet).
	const float Low1 = FPMLowPercentileMs(LowWindow, 1.0f);
	const float Low01 = FPMLowPercentileMs(LowWindow, 0.1f);
	Ar.Logf(TEXT("[FPM]   1%% low %s, 0.1%% low %s, over a %d-sample window. TELEMETRY ONLY -- "
	             "nothing steers on these."),
		Low1 < 0.0f ? TEXT("<not enough samples>") : *FString::Printf(TEXT("%.1f ms"), Low1),
		Low01 < 0.0f ? TEXT("<not enough samples>") : *FString::Printf(TEXT("%.1f ms"), Low01),
		LowWindow.Num());

	FFPMSteeringInputs Inputs;
	FString Coverage;
	const bool bSteerable = BuildInputs(EFPMGovernorMode::Balanced, Inputs, Coverage);
	Ar.Logf(TEXT("[FPM]   %s"), *Coverage);
	Ar.Logf(TEXT("[FPM]   %s"), bSteerable
		? TEXT("the mean is usable; what is missing is attribution, a resolution executor and a bench.")
		: TEXT("not yet usable for steering."));
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void FFPMSteerSignal::Arm()
{
	MeanMs = -1.0f;
	LastSampleSeconds = 0.0;
	SamplesSeen = 0;
	WorstSampleMs = 0.0f;
	LowWindow.Reset();
	LowWindowNext = 0;
	LastBudgets = FPMDeriveBudgets(GSteerTargetFPS.GetValueOnGameThread(), SteerReadMaxFPS());

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFPMSteerSignal::Tick), 0.0f);

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] steering signal armed. It MEASURES and reports; it decides nothing and writes no "
		     "console variable. FPM.Steer.Report prints the mean, the budgets and, in the same "
		     "breath, the three inputs it cannot supply."));
}

void FFPMSteerSignal::OnWorldLoad(UWorld* World)
{
	// A world change is a different scene: the mean from the last one describes nothing here, and a
	// loading screen's frames are already in it. Re-priming is cheaper and more honest than carrying
	// a number across the boundary and hoping the EMA forgets it.
	MeanMs = -1.0f;
	LastSampleSeconds = 0.0;
	SamplesSeen = 0;
	WorstSampleMs = 0.0f;
	LowWindow.Reset();
	LowWindowNext = 0;

	bSelfTestPassed = SelfTest();

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] steering signal: self-test %s; mean re-primed for the new world."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

void FFPMSteerSignal::Disarm()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	bSelfTestPassed = false;
}

static FAutoConsoleCommandWithOutputDevice GFPMSteerReportCmd(
	TEXT("FPM.Steer.Report"),
	TEXT("The governor's steering signal: smoothed frame mean, the derived budgets and their "
	     "derivation, the 1% low telemetry, and what the signal cannot supply. Reads only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMSteerSignal::Get().ReportNow(Ar);
	}));
