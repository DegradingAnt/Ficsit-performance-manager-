// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMGlassQuality.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGGameUserSettings.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * ★ THE TOGGLE. Ant, 2026-08-10: *"for now the main mod will just have a 'better glass' toggle."*
	 *
	 * Default 1. *"make it so the mod turns it on and keeps it on"* — so the shipped state is ON, and a
	 * player who wants vanilla glass turns it off rather than the other way round.
	 */
	TAutoConsoleVariable<int32> CVarGlassEnable(
		TEXT("FPM.Glass.Enable"), 1,
		TEXT("Hold Lumen front-layer translucency reflections ON, so glass and windows reflect properly "
		     "at every reflection-quality level. 0 releases both holds and returns to whatever the game "
		     "last set. Default 1."),
		ECVF_Default);

	const FName GGlassOwner(TEXT("glass-quality"));

	/**
	 * BOTH, ALWAYS, AND IN THIS ORDER. `.Allow` first because it is the AND term in the engine's gate —
	 * setting `.Enable` while `.Allow` is still 0 leaves a window, however short, where the readback says
	 * enabled and the renderer disagrees. There is no window the other way round.
	 */
	const TCHAR* const GGlassAllow  = TEXT("r.Lumen.TranslucencyReflections.FrontLayer.Allow");
	const TCHAR* const GGlassEnable = TEXT("r.Lumen.TranslucencyReflections.FrontLayer.Enable");

	int32 GHolds        = 0;   // successful Hold pairs applied
	int32 GRefusals     = 0;   // the writer said no — the reason is in ITS log line, not invented here
	int32 GReleases     = 0;   // toggled off, or disarmed
	int32 GVerifications = 0;  // settings-apply events where we read both values back
	int32 GRepairs      = 0;   // ... of which the value had been stomped and we put it back

	/** -1 when the cvar does not exist at all, which is a different answer from 0. */
	int32 ReadCVar(const TCHAR* Name)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
		return Var != nullptr ? Var->GetInt() : -1;
	}

	bool GlassWanted()
	{
		return CVarGlassEnable.GetValueOnGameThread() != 0;
	}
}

FFPMGlassQuality& FFPMGlassQuality::Get()
{
	static FFPMGlassQuality Instance;
	return Instance;
}

void FFPMGlassQuality::ApplyFromToggle(const TCHAR* Reason)
{
	if (!GlassWanted())
	{
		const bool bHadAllow  = FPMCVarWriter::Get().Release(GGlassOwner, GGlassAllow);
		const bool bHadEnable = FPMCVarWriter::Get().Release(GGlassOwner, GGlassEnable);

		if (bHadAllow || bHadEnable)
		{
			++GReleases;
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] glass: OFF (%s). Both holds released - the cvars return to whatever the game "
				     "last set, and FPM has written nothing anywhere that survives this."), Reason);
		}
		return;
	}

	const bool bAllow = FPMCVarWriter::Get().Hold(
		GGlassOwner, GGlassAllow, TEXT("1"),
		TEXT("better glass: .Allow is the AND term in the engine's front-layer gate"));

	const bool bEnable = FPMCVarWriter::Get().Hold(
		GGlassOwner, GGlassEnable, TEXT("1"),
		TEXT("better glass: .Enable feeds the first clause of the same gate"));

	if (!bAllow || !bEnable)
	{
		/*
		 * ⚠ PARTIAL IS WORSE THAN NEITHER, so say which half failed and do not pretend the feature is on.
		 * The writer already logged WHY it refused; repeating a guess here would just add a second,
		 * less-informed explanation beside the real one.
		 */
		++GRefusals;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] glass: the writer REFUSED %s%s%s. Its reason is the line above this one. Glass "
			     "quality is NOT applied - a half-applied pair is off, because .Allow ANDs .Enable."),
			bAllow  ? TEXT("") : TEXT(".Allow"),
			(!bAllow && !bEnable) ? TEXT(" and ") : TEXT(""),
			bEnable ? TEXT("") : TEXT(".Enable"));
		return;
	}

	++GHolds;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] glass: ON (%s). Holding .Allow=%d and .Enable=%d, read back from the engine. "
		     "⚠ Every settings-menu Apply will now log two LogConsoleManager warnings saying a "
		     "SetByScalability write on these two was ignored - those are OURS and they mean the hold "
		     "won (ConsoleManager.cpp:267-272)."),
		Reason, ReadCVar(GGlassAllow), ReadCVar(GGlassEnable));
}

void FFPMGlassQuality::Arm()
{
	if (ApplyHookHandle.IsValid()) { return; }

	ApplyFromToggle(TEXT("armed"));

	/*
	 * Live toggling. Without this, FPM.Glass.Enable would only take effect at the next boot, and a
	 * console setting that silently does nothing until restart is the shape that wastes one of her boots.
	 */
	if (IConsoleVariable* Toggle = CVarGlassEnable.AsVariable())
	{
		Toggle->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda(
			[](IConsoleVariable*) { FFPMGlassQuality::ApplyFromToggle(TEXT("FPM.Glass.Enable changed")); }));
	}

	/*
	 * ★ MEASURED 2026-08-10, AND THE ANSWER IS "THE RE-ASSERT HAS NEVER FIRED". FPM.Glass.Report, twice,
	 * in Ant's own session:
	 *
	 *     held through 2 settings apply(s) with 0 repairs. The priority argument holds:
	 *     ECVF_SetByPluginHighPriority (0x07) beats ECVF_SetByScalability (0x01)
	 *
	 * ⚠ IT IS KEPT ANYWAY, AND THAT IS A DELIBERATE REVERSAL of the "delete it, it is dead weight" note
	 * the report itself printed. Two applies is a thin sample, the hook costs nothing between settings
	 * changes, and deleting it would remove the only thing that can ever FALSIFY the priority argument.
	 * If a game or engine update changes that ladder, the version with this check logs a warning naming
	 * which cvar lost; the version without it silently stops making glass look right and nobody finds out
	 * until Ant notices. A free check that can disprove the mod's own reasoning earns its place.
	 *
	 * NAME THE LAMBDA FIRST — sf-scaffold section 7. SML's SUBSCRIBE_ macros split the handler on
	 * top-level commas, so a body is never written inside the macro.
	 */
	auto OnSettingsApplied = [](UFGGameUserSettings* Self)
	{
		if (!GlassWanted()) { return; }

		++GVerifications;

		const int32 Allow  = ReadCVar(GGlassAllow);
		const int32 Enable = ReadCVar(GGlassEnable);
		if (Allow == 1 && Enable == 1) { return; }

		/*
		 * ★ THE ARGUMENT WAS WRONG, AND THIS IS WHERE WE FIND OUT.
		 *
		 * The header reasons that a 0x07 hold cannot be stomped by a 0x01 scalability write. If that
		 * were true this branch is unreachable. Reaching it means something writes these at a priority
		 * at or above ECVF_SetByPluginHighPriority, and the value below names which cvar lost so the
		 * real stomp point can be found instead of guessed at.
		 */
		++GRepairs;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] glass: a settings apply STOMPED the hold - .Allow=%d .Enable=%d, both should be "
			     "1. Re-asserting. This should be impossible at ECVF_SetByPluginHighPriority, so the "
			     "stomp is coming from something at or above 0x07 and this counter is the evidence that "
			     "the re-assert is load-bearing (repair #%d of %d applies seen)."),
			Allow, Enable, GRepairs, GVerifications);

		FPMCVarWriter::Get().Release(GGlassOwner, GGlassAllow);
		FPMCVarWriter::Get().Release(GGlassOwner, GGlassEnable);
		FFPMGlassQuality::ApplyFromToggle(TEXT("re-assert after a settings apply"));
	};

	UFGGameUserSettings* Sample = GetMutableDefault<UFGGameUserSettings>();
	ApplyHookHandle = FPM_SUBSCRIBE_VIRTUAL_AFTER("glass-quality",
		UFGGameUserSettings::ApplyNonResolutionSettings, Sample, OnSettingsApplied);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] glass quality ARMED. Lumen front-layer translucency needs BOTH .Allow and .Enable - "
		     "BaseScalability.ini:393 sets .Allow=0 at sg.ReflectionQuality@2, which is why setting only "
		     ".Enable did nothing on High. No ini is written and nothing polls; the hold sits above "
		     "scalability in the cvar priority ladder, and a check after every settings apply proves "
		     "whether that is actually true. FPM.Glass.Report."));
}

void FFPMGlassQuality::Disarm()
{
	LogReport();

	if (ApplyHookHandle.IsValid())
	{
		// The gate's own reasoning applies here: an after-handler that keeps running past Disarm would
		// re-assert holds this fix has just released. Guarded on IsValid because the editor path returns
		// an invalid handle and SML's arrays were never allocated.
		UNSUBSCRIBE_METHOD(UFGGameUserSettings::ApplyNonResolutionSettings, ApplyHookHandle);
		ApplyHookHandle.Reset();
	}

	// Live and die with the mod, her words. The Module lease would release these at ShutdownModule
	// anyway; doing it here means a disarm without a shutdown also leaves nothing behind.
	const bool bAllow  = FPMCVarWriter::Get().Release(GGlassOwner, GGlassAllow);
	const bool bEnable = FPMCVarWriter::Get().Release(GGlassOwner, GGlassEnable);
	if (bAllow || bEnable) { ++GReleases; }
}

void FFPMGlassQuality::LogReport(FOutputDevice* Ar)
{
	const int32 Allow  = ReadCVar(GGlassAllow);
	const int32 Enable = ReadCVar(GGlassEnable);

	const FString Line = FString::Printf(
		TEXT("[FPM] glass: toggle=%s  .Allow=%d  .Enable=%d  ->  reflections on glass are %s. "
		     "%d hold(s) applied, %d refused, %d released."),
		GlassWanted() ? TEXT("ON") : TEXT("OFF"), Allow, Enable,
		(Allow == 1 && Enable == 1) ? TEXT("ACTIVE") : TEXT("not active"),
		GHolds, GRefusals, GReleases);

	/*
	 * ★ THE VERDICT LINE, AND IT REFUSES TO SAY "ALL CLEAR" ON NO DATA.
	 *
	 * Zero repairs out of zero verifications is not a passing grade, it is an unrun test. Printing it as
	 * success is the dead-instrument shape this project has paid for five times, so the three cases are
	 * spelled out and the zero-sample case names what to do about it.
	 */
	FString Verdict;
	if (GVerifications == 0)
	{
		Verdict = TEXT("[FPM]   the hold has never been CHECKED - no settings apply has happened yet this "
		               "session. Open the settings menu and press Apply, then run this again. Until then "
		               "the 0 repairs below is an unrun test, not a clean one.");
	}
	else if (GRepairs == 0)
	{
		Verdict = FString::Printf(
			TEXT("[FPM]   held through %d settings apply(s) with 0 repairs. The priority argument holds: "
			     "ECVF_SetByPluginHighPriority (0x07) beats ECVF_SetByScalability (0x01), so the "
			     "re-assert is dead weight and can be deleted."), GVerifications);
	}
	else
	{
		Verdict = FString::Printf(
			TEXT("[FPM]   %d of %d settings apply(s) STOMPED the hold. The priority argument is WRONG and "
			     "the re-assert is load-bearing - find what writes these at or above 0x07."),
			GRepairs, GVerifications);
	}

	if (Ar != nullptr)
	{
		Ar->Log(Line);
		Ar->Log(Verdict);
	}

	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Verdict);
}

/*
 * `FPM.Glass.Report` — takes the output device so it prints in the console she is looking at as well as
 * the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command that answers
 * somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GGlassReportCmd(
	TEXT("FPM.Glass.Report"),
	TEXT("Print whether Lumen front-layer translucency reflections are actually active, and whether the "
	     "hold has ever survived a settings apply."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMGlassQuality::LogReport(&Ar);
	}));
