// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMMasterSwitch.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMFixContract.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/*
	 * ⚠ EVERY WARNING GLYPH BELOW AND IN THIS FILE IS A LITERAL, NEVER `\xE2\x9A\xA0`.
	 *
	 * `TEXT()` makes a WIDE string, so those three bytes became three separate UTF-16 characters and
	 * the log printed "â " where the warning sign belonged. Her 2026-08-15 log has it at 20:33:01.935.
	 * `FPMFixContract.cpp` writes the same glyph as a literal and prints it correctly in the same log
	 * at 20:33:25.515, which is what settles it rather than an argument about encodings.
	 */
	static TAutoConsoleVariable<int32> CVarFPMEnabled(
		TEXT("FPM.Enabled"), 1,
		TEXT("Master switch. 1 = on (default), 0 = off.\n"
		     "OFF disarms every fix AND RELEASES every console variable FPM wrote, restoring each one's "
		     "prior value and prior SetBy priority - it does not merely stop writing new ones.\n"
		     "ON re-arms. ⚠ A fix whose work happens at world load comes back armed but INERT until "
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

	/**
	 * Item 2's registry. A FUNCTION-LOCAL static, not a namespace-scope one - the only other caller,
	 * FPMCVarProbe.cpp's static registration, lives in a DIFFERENT translation unit, and static
	 * initialization order across TUs is unspecified. Construct-on-first-use sidesteps that instead
	 * of gambling on link order.
	 */
	TArray<TFunction<void()>>& GetStopHooks()
	{
		static TArray<TFunction<void()>> Hooks;
		return Hooks;
	}

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
				TEXT("[FPM] MASTER SWITCH: OFF. Stopping any diagnostic run, disarming every fix, then "
				     "releasing every console variable FPM wrote."));

			// FIRST: item 2. FPM.Bisect / FPM.Prove write outside the writer's tagged hold (the
			// accepted ECVF_SetByConsole exception), so ReleaseAll below cannot reach an active run.
			for (const TFunction<void()>& Hook : GetStopHooks())
			{
				Hook();
			}

			FPMFixes::DisarmAll();
			FPMCVarWriter::Get().ReleaseAll(TEXT("FPM.Enabled 0"));

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: OFF complete. FPM.Enabled 1 turns it back on. ⚠ Fixes "
				     "whose work happens at world load will come back armed but inert until the next "
				     "one."));
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: ON. Re-arming."));

			/*
			 * ★ ARM TO THE DESIRED STATE DIRECTLY. DO NOT `RearmAll()` FIRST.
			 *
			 * This used to be `FPMFixes::RearmAll()` followed by `ReapplyAll()`. `RearmAll` arms
			 * EVERYTHING in the registry, so the pair armed every opt-out fix and then disarmed it
			 * again a few hundred microseconds later. The end state was right and the route was not.
			 *
			 * ⚠ MEASURED IN ANT'S 2026-08-15 LOG, not reasoned about. Two `FPM.Enabled 0` -> `1` cycles
			 * (20:33:25 and 20:53:53) each produced `re-armed 50 of 50 registered fix(es)` followed by
			 * eight `DISARMED by toggle` lines, the eight `DefaultArmed() == false` fixes. Three of
			 * those eight install hooks, and each cycle therefore installed and immediately removed a
			 * funchook detour on `UNetDriver::ProcessRemoteFunction`, `FNetGUIDCache::SupportsObject`
			 * and `UFGGameUserSettings::ApplyNonResolutionSettings`. `ProcessRemoteFunction` is the
			 * universal RPC path; patching it twice for nothing is not free, and the gate is OFF BY
			 * DEFAULT precisely because it was measured not worth its detour.
			 *
			 * ⚠ AND IT CORRUPTED AN INSTRUMENT PERMANENTLY. `FPMHookLedger` has no retire path, so each
			 * transient install left a row behind that no armed fix owns. After two cycles her log's
			 * `FPM.Hooks.Report` carried six such rows and printed `coverage: 3 hook owner(s) match no
			 * ARMED fix: no-owner-rpc-gate, net-guid-census, upscaler-preset`, a true warning about a
			 * fault this line created. The ledger's own defect is separate and still open; this stops
			 * feeding it.
			 *
			 * The per-fix toggles were ALREADY the authority here: `ReapplyAll` ran second and won.
			 * Asking them alone reaches the same end state without the round trip.
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

	/*
	 * ★ PROVE THE REGISTRY BEFORE THE SWITCH IS ALLOWED TO USE IT.
	 *
	 * Same discipline as `FPMCVarWriter::SelfTest()` at the top of StartupModule: the switch's whole
	 * safety rests on SetArmed refusing unregistered fixes and on the three accessors agreeing, and
	 * neither is worth asserting when both can be checked in microseconds every boot.
	 */
	FPMFixes::SelfTest();

	// If the launch environment already asked for OFF, honour it now rather than at the first toggle.
	ApplyMasterSwitch();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] master switch installed: FPM.Enabled %d. OFF disarms every fix and RELEASES every "
		     "console variable FPM wrote - it does not just stop writing."),
		CVarFPMEnabled.GetValueOnGameThread());
}

void FPMMasterSwitch::RegisterStopHook(TFunction<void()> Hook)
{
	GetStopHooks().Add(MoveTemp(Hook));
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

		// ONE DECLARATION SITE for the default: FPMFixes::Arm reads the same predicate for the initial
		// install. A second copy here would be the two-defaults bug this project has already shipped once.
		const int32 DefaultValue = Fix->DefaultArmed() ? 1 : 0;

		IConsoleVariable* Var = Console.RegisterConsoleVariable(
			*VarName, DefaultValue,
			*FString::Printf(
				TEXT("Arm or disarm the '%s' fix. 1 = armed, 0 = disarmed and its hooks removed. "
				     "Default for this fix is %d. Subordinate to FPM.Enabled: while that is 0, setting "
				     "this to 1 is refused and the value applies when the master switch returns."),
				*FixName, DefaultValue),
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
	int32 Armed = 0;
	int32 HeldOff = 0;
	int32 NoToggle = 0;

	for (IFPMFix* Fix : FPMFixes::Registered())
	{
		if (Fix == nullptr) { continue; }

		if (IConsoleVariable** Var = GFPMFixToggleCVars.Find(FString(Fix->Name())))
		{
			ApplyOneToggle(Fix, *Var);
		}
		else if (FPMMasterSwitch::IsEnabled())
		{
			/*
			 * ⚠ NO TOGGLE MEANS NO OPINION, NOT "LEAVE IT OFF". `Install()` skips a fix whose cvar
			 * failed to register (a name collision, logged there), and this is the only other route
			 * back to armed after the master switch turned everything off. Without this branch such a
			 * fix would stay disarmed for the rest of the session while `FPM.Fix.List` showed no reason
			 * why. `FPMFixes::RearmAll()` used to be what covered it and the ON path no longer calls
			 * that. Falling back to `DefaultArmed()` gives the fix the same state boot gave it.
			 *
			 * ⚠ GATED ON THE MASTER SWITCH, exactly as `ApplyOneToggle` is. Today the only caller is the
			 * ON path, so the guard cannot fire. It is here because a future caller that forgets would
			 * otherwise arm a fix inside a mod the user has turned OFF, and that is the one failure this
			 * whole namespace exists to refuse.
			 */
			++NoToggle;
			FPMFixes::SetArmed(*Fix, Fix->DefaultArmed());
		}

		if (FPMFixes::IsArmed(*Fix)) { ++Armed; } else { ++HeldOff; }
	}

	/*
	 * ★ SAY WHAT WAS NOT ARMED, because this is the line that replaced `RearmAll`'s own "re-armed N of
	 * M" and a quieter replacement would read as a smaller job. After `FPM.Enabled 1` the mod is NOT
	 * fully on, and a reader deciding whether FPM is running needs the second number to see that.
	 *
	 * ⚠ "HELD OFF" NAMES NO CAUSE, DELIBERATELY. A fix can end this loop disarmed because its own
	 * toggle reads 0, or because `ApplyOneToggle` refused a 1 while the master switch is off. Today
	 * only the ON path calls this, so it is always the first, but a line that asserts the reason would
	 * be asserting something this function does not actually distinguish. `FPM.Fix.List` prints each
	 * toggle beside its armed state and answers the "which and why" properly.
	 *
	 * ⚠ The OnWorldLoad caveat is carried over verbatim from `RearmAll`. Re-arming does not replay a
	 * world load, so a fix whose work happens there is armed and inert until the next one, and dropping
	 * that sentence would have made this report half-true.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] fix toggles applied: %d armed, %d held OFF, %d had no toggle and fell back to its "
		     "default. FPM.Fix.List names them. ⚠ Fixes whose work happens in OnWorldLoad are armed but "
		     "INERT until the next world load - this does not replay one."),
		Armed, HeldOff, NoToggle);
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
