// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMMasterSwitch.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMFixContract.h"

#include "HAL/IConsoleManager.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarFPMEnabled(
		TEXT("FPM.Enabled"), 1,
		TEXT("Master switch. 1 = on (default), 0 = off.\n"
		     "OFF disarms every fix AND RELEASES every console variable FPM wrote, restoring each one's "
		     "prior value and prior SetBy priority - it does not merely stop writing new ones.\n"
		     "ON re-arms. \xE2\x9A\xA0 A fix whose work happens at world load comes back armed but INERT until "
		     "the next world load; re-arming does not replay one."),
		ECVF_Default);

	/**
	 * What the switch last ACTED on.
	 *
	 * ⚠ NOT a cache of the cvar — a record of the last transition performed. The sink fires on every
	 * `set`, including one that writes the value it already had, and acting on that would call
	 * `DisarmAll` twice in a row. The second call would find an empty armed list and report a disarm
	 * that disarmed nothing, which is a false line in a log people read to decide whether FPM is
	 * running.
	 */
	bool bGFPMLastAppliedEnabled = true;

	bool bGFPMSwitchInstalled = false;

	void ApplyMasterSwitch()
	{
		const bool bWanted = CVarFPMEnabled.GetValueOnGameThread() != 0;
		if (bWanted == bGFPMLastAppliedEnabled)
		{
			return;
		}
		bGFPMLastAppliedEnabled = bWanted;

		if (!bWanted)
		{
			/*
			 * ★ ORDER MATTERS AND THIS IS THE ORDER.
			 *
			 * Disarm FIRST, release SECOND. A fix that re-asserts a console variable from a hook — the
			 * glass quality hold is exactly that, it re-applies on a settings apply — would otherwise
			 * write its value straight back in the window between the release and its own teardown, and
			 * the release would silently accomplish nothing on precisely the levers that fight hardest
			 * to stay set.
			 */
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: OFF. Disarming every fix, then releasing every console "
				     "variable FPM wrote."));

			FPMFixes::DisarmAll();
			FPMCVarWriter::Get().ReleaseAll(TEXT("FPM.Enabled 0"));

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: OFF complete. FPM.Enabled 1 turns it back on. \xE2\x9A\xA0 Fixes "
				     "whose work happens at world load will come back armed but inert until the next "
				     "one."));
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: ON. Re-arming."));

			FPMFixes::RearmAll();

			/*
			 * ★ AND THEN HONOUR THE PER-FIX TOGGLES AGAIN, which is not optional.
			 *
			 * `RearmAll` arms EVERYTHING in the registry — including fixes the user had individually
			 * turned off before flipping the master switch. Without this line, `FPM.Enabled 0` followed
			 * by `FPM.Enabled 1` would silently re-enable every fix the user had disabled, while their
			 * toggles still read 0. The settings surface would then be lying about the running state.
			 */
			FPMFixToggles::ReapplyAll();
		}
	}
}

void FPMMasterSwitch::Install()
{
	if (bGFPMSwitchInstalled) { return; }
	bGFPMSwitchInstalled = true;

	/*
	 * ⚠ SEEDED FROM THE LIVE CVAR RATHER THAN ASSUMED TRUE.
	 *
	 * `Install()` runs at the end of StartupModule, by which point everything has already armed — so the
	 * real state is "on". But the cvar can be set from the command line or an ini BEFORE this point, and
	 * seeding a hardcoded `true` against a cvar that already reads 0 would leave the switch believing
	 * FPM is on while the user asked for off, and the first `set` back to 0 would be swallowed as a
	 * no-op transition.
	 */
	bGFPMLastAppliedEnabled = true;
	CVarFPMEnabled.AsVariable()->SetOnChangedCallback(
		FConsoleVariableDelegate::CreateStatic([](IConsoleVariable*) { ApplyMasterSwitch(); }));

	/*
	 * Toggles BEFORE the first ApplyMasterSwitch, because ApplyOneToggle asks the master switch whether
	 * FPM is enabled — registering them after would mean a launch-time `FPM.Enabled 0` disarmed
	 * everything while no toggle yet existed to record it.
	 */
	FPMFixToggles::Install();

	// If the launch environment already asked for OFF, honour it now rather than at the first toggle.
	ApplyMasterSwitch();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] master switch installed: FPM.Enabled %d. OFF disarms every fix and RELEASES every "
		     "console variable FPM wrote - it does not just stop writing."),
		CVarFPMEnabled.GetValueOnGameThread());
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// P4.3 — per-fix toggles
// ─────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
	/** Fix name -> its generated cvar, so ReapplyAll and the listing never re-derive the mapping. */
	TMap<FString, IConsoleVariable*> GFPMFixToggleCVars;

	/**
	 * `static-base` -> `StaticBase`.
	 *
	 * ⚠ NOT the fix name verbatim. No engine console variable contains a hyphen, and `ConsoleManager.cpp`
	 * validates nothing — so a hyphenated name would be accepted at registration and might only misbehave
	 * later, at the parser. Deriving a conventional name costs one loop and removes the guess entirely.
	 */
	FString ToCVarSuffix(const FString& FixName)
	{
		FString Out;
		Out.Reserve(FixName.Len());
		bool bUpperNext = true;
		for (const TCHAR C : FixName)
		{
			if (C == TEXT('-') || C == TEXT('_') || C == TEXT(' '))
			{
				bUpperNext = true;
				continue;
			}
			Out.AppendChar(bUpperNext ? FChar::ToUpper(C) : C);
			bUpperNext = false;
		}
		return Out;
	}

	void ApplyOneToggle(IFPMFix* Fix, IConsoleVariable* Var)
	{
		if (Fix == nullptr || Var == nullptr) { return; }

		const bool bWant = Var->GetInt() != 0;

		/*
		 * ★ THE MASTER SWITCH WINS, AND THE REFUSAL IS LOUD.
		 *
		 * Arming one fix while the user has FPM turned off would be a settings surface quietly doing the
		 * opposite of what it says. The toggle keeps its value, so it takes effect on the next
		 * FPM.Enabled 1 through ReapplyAll.
		 */
		if (bWant && !FPMMasterSwitch::IsEnabled())
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] fix toggle '%s' = 1 REFUSED while FPM.Enabled is 0. The value is kept and "
				     "will apply when the master switch goes back on."), Fix->Name());
			return;
		}

		if (FPMFixes::SetArmed(*Fix, bWant))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] fix '%s' %s by toggle."), Fix->Name(), bWant ? TEXT("ARMED") : TEXT("DISARMED"));
		}
	}
}

void FPMFixToggles::Install()
{
	IConsoleManager& Console = IConsoleManager::Get();

	for (IFPMFix* Fix : FPMFixes::Registered())
	{
		if (Fix == nullptr) { continue; }

		const FString FixName = Fix->Name();
		const FString VarName = FString::Printf(TEXT("FPM.Fix.%s"), *ToCVarSuffix(FixName));

		IConsoleVariable* Var = Console.RegisterConsoleVariable(
			*VarName, 1,
			*FString::Printf(
				TEXT("Arm or disarm the '%s' fix. 1 = armed (default), 0 = disarmed and its hooks "
				     "removed. Subordinate to FPM.Enabled: while that is 0, setting this to 1 is "
				     "refused and the value applies when the master switch returns."), *FixName),
			ECVF_Default);

		if (Var == nullptr)
		{
			// A name collision is the only realistic cause, and it must not pass silently — the toggle
			// would simply not exist while the listing implied it did.
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] could not register toggle '%s' for fix '%s'. That fix has NO toggle."),
				*VarName, *FixName);
			continue;
		}

		GFPMFixToggleCVars.Add(FixName, Var);
		Var->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda(
			[Fix](IConsoleVariable* Changed) { ApplyOneToggle(Fix, Changed); }));
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] %d per-fix toggle(s) registered from the fix registry - no hand-written list. "
		     "FPM.Fix.List prints each fix name beside its generated cvar."), GFPMFixToggleCVars.Num());
}

void FPMFixToggles::ReapplyAll()
{
	for (IFPMFix* Fix : FPMFixes::Registered())
	{
		if (Fix == nullptr) { continue; }
		if (IConsoleVariable** Var = GFPMFixToggleCVars.Find(FString(Fix->Name())))
		{
			ApplyOneToggle(Fix, *Var);
		}
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMFixListCmd(
	TEXT("FPM.Fix.List"),
	TEXT("List every fix, its generated toggle cvar, and whether it is armed right now."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		Ar.Logf(TEXT("[FPM] master: FPM.Enabled %d"), FPMMasterSwitch::IsEnabled() ? 1 : 0);
		for (IFPMFix* Fix : FPMFixes::Registered())
		{
			if (Fix == nullptr) { continue; }
			Ar.Logf(TEXT("[FPM]   %-28s  %-34s  %s"),
				Fix->Name(),
				*FString::Printf(TEXT("FPM.Fix.%s"), *ToCVarSuffix(FString(Fix->Name()))),
				FPMFixes::IsArmed(*Fix) ? TEXT("armed") : TEXT("DISARMED"));
		}
		Ar.Logf(TEXT("[FPM]   %d registered. Names are DERIVED - hyphens removed, next letter "
		             "capitalised - because no engine cvar uses a hyphen."),
			FPMFixes::Registered().Num());
	}));

bool FPMMasterSwitch::IsEnabled()
{
	// The live cvar, never a cached copy: a caller asking "is FPM on" during a transition must not be
	// told what the switch last ACTED on.
	return CVarFPMEnabled.GetValueOnGameThread() != 0;
}
