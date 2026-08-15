// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMSaveSettingsInterceptor.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMUserSettingMap.h"

#include "FGGameUserSettings.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/** Set only by a successful Arm(). Until then IsHealthy() is false and mapped writes are refused. */
	bool bGFPMSaveGuardArmed = false;

	/**
	 * ⚠ LATCHING, AND IT NEVER UNLATCHES. Once this is true every mapped write is refused for the rest of
	 * the session. There is no retry and no self-healing: a guard that recovers on its own is a guard
	 * nobody investigates, and the failure it is covering here ends in a permanent edit to the player's
	 * settings file.
	 */
	bool bGFPMSaveGuardFailed = false;
	FString GFPMSaveGuardFailReason;

	int32 GFPMSaveGuardSavesSeen = 0;
	int32 GFPMSaveGuardHoldsSuspended = 0;

	/**
	 * Holds stood down for the CURRENT save, waiting for the after-hook to put them back.
	 *
	 * ⚠ IF THIS IS NON-EMPTY WHEN THE BEFORE-HOOK RUNS, THE PREVIOUS SAVE NEVER COMPLETED ITS AFTER-HOOK
	 * and FPM is currently standing down when it thinks it is holding. That is a fail-safe trip, not a
	 * situation to tidy up quietly.
	 */
	TArray<FPMCVarWriter::FHoldView> GFPMSaveGuardSuspended;

	bool SaveGuardSay(int32 Level = 1)
	{
		return FPMDiag::IsOn(FPMDiag::EChannel::SaveGuard, Level);
	}

	/**
	 * ★ THE USER-SETTING MAP'S LIVENESS PROOF — mirrors FPMResidueSentinel.cpp's
	 * FPMSentinelClassifierIsAlive() (Ant, 2026-08-09: *"dead instruments are not my preferred item to
	 * exist."*), same pattern, same reason: the before-hook's filter is `FPMUserSettingMap::IsBacked`,
	 * so a map that went empty (the generated `FPMUserSettingTable.g.h` AND the legacy fallback array
	 * both dead) would answer "not backed" for everything, stand NOTHING down before a save, and this
	 * self-test would still see two valid hook handles and arm reporting HEALTHY. A held US_*-backed
	 * cvar would then leak permanently into GameUserSettings.ini - the exact harm this file exists to
	 * stop - with every log line involved reading clean.
	 *
	 * A check that merely calls `IsBacked` and expects it not to crash proves nothing; the thing in
	 * doubt is whether it can still tell the two cases apart. So a known-positive must return true and a
	 * known-negative must return false, same two probes the sentinel already uses:
	 *   - known-positive: `t.MaxFPS`, a real US_MaxFPS StrId confirmed cvar-backed (FPMCVarWriter.h:30).
	 *   - known-negative: `FPMCVarWriter::SelfTestProbeName()`, FPM's own scratch cvar, which is not any
	 *     game setting's StrId and so must never classify as backed.
	 */
	bool FPMSaveGuardUserSettingMapIsAlive()
	{
		const bool bPositive = FPMUserSettingMap::IsBacked(TEXT("t.MaxFPS"));
		const bool bNegative = FPMUserSettingMap::IsBacked(FPMCVarWriter::SelfTestProbeName());
		if (bPositive && !bNegative) { return true; }

		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ THE USER-SETTING MAP IS DEAD: 't.MaxFPS' classified %s (expected backed) and "
			     "'%s' classified %s (expected NOT backed). IsBacked() cannot discriminate, so the "
			     "before-hook's filter would stand NOTHING down while this guard reports healthy - a held "
			     "US_*-backed cvar would then be serialised into GameUserSettings.ini permanently."),
			bPositive ? TEXT("backed") : TEXT("NOT backed"),
			FPMCVarWriter::SelfTestProbeName(),
			bNegative ? TEXT("BACKED") : TEXT("not backed"));
		return false;
	}
}

FFPMSaveSettingsInterceptor& FFPMSaveSettingsInterceptor::Get()
{
	static FFPMSaveSettingsInterceptor Instance;
	return Instance;
}

bool FFPMSaveSettingsInterceptor::IsHealthy()
{
	// FAIL CLOSED on all three: not armed yet, armed-and-failed, or mid-suspension. "We cannot tell" is
	// not "it is fine" — the jq-hook lesson, applied to the one guard whose failure is permanent.
	return bGFPMSaveGuardArmed && !bGFPMSaveGuardFailed && GFPMSaveGuardSuspended.Num() == 0;
}

void FFPMSaveSettingsInterceptor::Fail(const FString& Reason)
{
	if (bGFPMSaveGuardFailed) { return; }   // first reason wins; later ones are consequences

	bGFPMSaveGuardFailed = true;
	GFPMSaveGuardFailReason = Reason;

	/*
	 * ⚠ NOT GATED BY THE DIAG CHANNEL. Everything else in this file is; this is not. The channel exists
	 * so a reader can quieten routine chatter, and a permanent fail-safe trip is not routine chatter —
	 * silencing it would mean the mod refuses every mapped write for the rest of the session while the
	 * log says nothing about why. That is the exact shape FPMDiag.h calls out as a broken contract, and
	 * this is the stated exception, stated here.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Error,
		TEXT("[FPM] save-settings guard FAILED PERMANENTLY: %s\n"
		     "       Every write to a US_*-backed cvar is now refused for the rest of this session. That "
		     "is the safe direction: without this guard a held value would be serialised into "
		     "GameUserSettings.ini and would survive uninstalling the mod.\n"
		     "       REMEDY: restart the game. If it recurs, the SaveSettings hook target has moved - "
		     "check UFGGameUserSettings::SaveSettings against FGGameUserSettings.h and report the game "
		     "build."),
		*Reason);
}

void FFPMSaveSettingsInterceptor::GetCounts(int32& OutSavesSeen, int32& OutHoldsSuspended)
{
	OutSavesSeen = GFPMSaveGuardSavesSeen;
	OutHoldsSuspended = GFPMSaveGuardHoldsSuspended;
}

void FFPMSaveSettingsInterceptor::Arm()
{
	/*
	 * ⚠ A COOK IS NOT A GAME. Do not arm, and do NOT latch a failure. Found 2026-08-09 the hard way:
	 * this guard BROKE PACKAGING.
	 *
	 * WHAT HAPPENED. The cook commandlet loads this module, `Arm()` ran, the `UFGGameUserSettings`
	 * SaveSettings hook could not install in a headless commandlet, and the permanent fail-safe did
	 * exactly what it was designed to do: latch FAILED and log at Error level. The cook counts Errors and
	 * fails on them, so `RunUAT` came back `Error_UnknownCookFailure` (ExitCode 25) and produced NO new
	 * artefacts — while still printing enough success-shaped output that only a byte check on the packaged
	 * `.uplugin` revealed the zips were the previous version.
	 *
	 * WHY SKIPPING IS CORRECT AND NOT A WORKAROUND. This guard exists to stop a TRANSIENT cvar write from
	 * being serialised into a player's settings file. A cook has no player, no settings file and no save;
	 * there is no write to intercept and nothing to protect. Arming would guard nothing, and its FAILURE
	 * to arm therefore says nothing about the guard's health in a real session.
	 *
	 * WHY IT IS A PLAIN RETURN AND NOT `Fail()`. `Fail()` is permanent and never re-arms by design. Using
	 * it here would mean a cook's irrelevant miss poisons the very next real boot in the same process. The
	 * safety posture is unchanged: `IsHealthy()` still fails CLOSED before arming, so any mapped write in
	 * this context is refused anyway. Not-armed and not-healthy is the safe pair.
	 */
	if (IsRunningCommandlet())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] save-settings guard: not arming in a commandlet (cook/build). There is no player "
			     "settings file to protect here, and IsHealthy() stays false, so mapped writes stay refused."));
		return;
	}

	/*
	 * INVARIANT 3 — REFUSE TO ARM WHILE HELD (§2.3.6).
	 *
	 * Arming happens in StartupModule, where the writer's ledger cannot yet contain anything, so this is
	 * expected to be trivially true. It is CHECKED anyway. "That cannot happen" is precisely how the
	 * 0x2c0 schematic guard was justified three separate times, and each time something had changed.
	 */
	TArray<FString> AlreadyHeld;
	FPMCVarWriter::Get().GetHeldCVars(AlreadyHeld);
	if (AlreadyHeld.Num() > 0)
	{
		Fail(FString::Printf(
			TEXT("%d cvar(s) were already held at arm time (first: %s). The guard would be protecting a "
			     "write it never saw go up."),
			AlreadyHeld.Num(), *AlreadyHeld[0]));
		return;
	}

	UFGGameUserSettings* Sample = GetMutableDefault<UFGGameUserSettings>();
	if (!Sample)
	{
		Fail(TEXT("could not reach the UFGGameUserSettings CDO to install the hook."));
		return;
	}

	/*
	 * BEFORE THE SAVE — stand our mapped holds down so the game serialises the PLAYER's values.
	 *
	 * Release is the engine's tagged-history Unset, so the cvar falls back to whatever layer the player's
	 * own setting occupies. We deliberately do NOT restore a remembered value: a remembered value can be
	 * our own earlier write, which is the ratchet R33 removed baseline-capture to kill.
	 */
	auto OnSaveBefore = [](auto& Scope, UFGGameUserSettings* Self)
	{
		++GFPMSaveGuardSavesSeen;

		if (GFPMSaveGuardSuspended.Num() > 0)
		{
			// The previous save's after-hook never ran. We are standing down while believing we hold.
			Fail(FString::Printf(
				TEXT("a save began while %d hold(s) were still suspended from a previous save - the "
				     "after-hook did not run."), GFPMSaveGuardSuspended.Num()));
			return;
		}

		if (bGFPMSaveGuardFailed) { return; }

		TArray<FPMCVarWriter::FHoldView> Live;
		FPMCVarWriter::Get().GetHolds(Live);

		for (const FPMCVarWriter::FHoldView& H : Live)
		{
			if (!FPMUserSettingMap::IsBacked(*H.CVar)) { continue; }   // not capturable; leave it alone

			if (!FPMCVarWriter::Get().Release(H.Owner, *H.CVar))
			{
				Fail(FString::Printf(TEXT("could not release '%s' (owner '%s') before the save."),
					*H.CVar, *H.Owner.ToString()));
				return;
			}
			GFPMSaveGuardSuspended.Add(H);
			++GFPMSaveGuardHoldsSuspended;
		}

		if (GFPMSaveGuardSuspended.Num() > 0 && SaveGuardSay())
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] save-settings guard: stood %d hold(s) down for save #%d so the game "
				     "serialises the player's values, not ours."),
				GFPMSaveGuardSuspended.Num(), GFPMSaveGuardSavesSeen);
		}
		else if (SaveGuardSay(2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] save-settings guard: save #%d, nothing held that the save would capture."),
				GFPMSaveGuardSavesSeen);
		}
	};

	/*
	 * AFTER THE SAVE — put them back exactly as they were, through the ordinary public Hold path.
	 *
	 * ⚠ NO `Scope` PARAMETER, unlike the before-hook. The _AFTER variant's handler is
	 * `TFunction<void(Args...)>` — the body has already run, so there is nothing left to cancel or
	 * override and SML does not hand you a scope. Writing it with one is a compile error (C2664), which
	 * is the good outcome: the shape of the callback tells you that an after-hook cannot change the call.
	 */
	auto OnSaveAfter = [](UFGGameUserSettings* Self)
	{
		if (GFPMSaveGuardSuspended.Num() == 0) { return; }

		const int32 Count = GFPMSaveGuardSuspended.Num();
		int32 Restored = 0;

		for (const FPMCVarWriter::FHoldView& H : GFPMSaveGuardSuspended)
		{
			if (FPMCVarWriter::Get().Hold(H.Owner, *H.CVar, *H.Value, *H.Reason, H.Lease)) { ++Restored; }
		}

		/*
		 * ⚠ CLEAR BEFORE JUDGING. If a re-apply failed we are about to latch the fail-safe, and leaving
		 * the array populated would make every subsequent save trip the "previous save never finished"
		 * branch as well — one fault reported forever, drowning its own cause.
		 */
		GFPMSaveGuardSuspended.Reset();

		if (Restored != Count)
		{
			Fail(FString::Printf(TEXT("re-applied only %d of %d hold(s) after the save."), Restored, Count));
			return;
		}

		if (SaveGuardSay())
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] save-settings guard: re-applied %d hold(s) after save #%d."),
				Restored, GFPMSaveGuardSavesSeen);
		}
	};

	const FDelegateHandle Before =
		SaveBeforeHandle = FPM_SUBSCRIBE_VIRTUAL("save-settings-guard", UFGGameUserSettings::SaveSettings, Sample, OnSaveBefore);
	const FDelegateHandle After =
		SaveAfterHandle = FPM_SUBSCRIBE_VIRTUAL_AFTER("save-settings-guard", UFGGameUserSettings::SaveSettings, Sample, OnSaveAfter);

	/*
	 * INVARIANT 1 — THE ARM-TIME SELF-TEST, and an honest statement of its reach.
	 *
	 * It checks the two things that can be checked without side effects: that BOTH hooks installed
	 * (below), and that the user-setting map can actually DISCRIMINATE
	 * (`FPMSaveGuardUserSettingMapIsAlive()`, below the hook check) — the before-hook's filter is
	 * `FPMUserSettingMap::IsBacked`, so a map that went blind would silently stand nothing down and this
	 * guard would arm reporting healthy while protecting nothing. Fixed 2026-08-15: until then this
	 * comment described a check the code never ran.
	 *
	 * ⚠ WHAT IT CANNOT DO, SAID OUT LOUD: it does not call SaveSettings. Calling it would write Ant's
	 * real GameUserSettings.ini, which is the exact harm this file exists to prevent — a self-test that
	 * causes the damage it tests for is not a test. So the end-to-end path is proven by a BOOT with a
	 * real save, not here, and this must never be reported as end-to-end verification.
	 */
	if (!Before.IsValid() || !After.IsValid())
	{
		/*
		 * ⚠ AN INVALID HANDLE IN AN EDITOR BUILD IS EXPECTED, NOT A FAULT — and reporting it as one is a
		 * defect this guard shipped with until 2026-08-10.
		 *
		 * `FPMHookLedger::Install` carries the editor gate and refuses EVERY hook in an editor build;
		 * the boot log shows all 22 as "REFUSED (editor build)". This branch could not tell that apart
		 * from the hook target genuinely having moved, so it logged an ERROR reading "FAILED
		 * PERMANENTLY" and told the reader to restart the game and re-check
		 * `UFGGameUserSettings::SaveSettings` against the header. Nothing was wrong, and the advice sent
		 * them looking for a bug that does not exist — on every single editor launch.
		 *
		 * ★ THE FAIL-SAFE STILL ENGAGES, and that part was always right. With no hook installed there is
		 * nothing standing FPM's holds down across a save, so refusing every US_*-backed write remains
		 * the correct posture. What changes is only the CLASSIFICATION and the remedy text: in the
		 * editor this is a designed no-op, in a shipping build it is a real fault.
		 */
		const FString Detail = FString::Printf(TEXT("hook install failed (before=%s, after=%s)."),
			Before.IsValid() ? TEXT("ok") : TEXT("FAILED"),
			After.IsValid()  ? TEXT("ok") : TEXT("FAILED"));

		/*
		 * ⚠ THE `else` IS LOAD-BEARING, NOT STYLE. Written first as an early `return` inside the
		 * `if constexpr`, which leaves `Fail()` unreachable in an editor build — and this project
		 * compiles warnings as errors, so C4702 stopped the build outright. The discarded branch of an
		 * `if constexpr` still has to leave live code after it, or there must be no code after it.
		 */
		if constexpr (WITH_EDITOR)
		{
			bGFPMSaveGuardFailed = true; // same fail-safe, different diagnosis

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] save-settings guard INERT in the editor, and this is BY DESIGN - not a fault. "
				     "%s The hook ledger's editor gate refuses every hook in an editor build (see the "
				     "'REFUSED (editor build)' lines above; all of them are refused, not just these two). "
				     "US_*-backed writes stay refused for this session, which is the correct posture with "
				     "no interceptor installed. Nothing to fix, and nothing to restart."), *Detail);
		}
		else
		{
			Fail(Detail);
		}
		return;
	}

	/*
	 * SECOND HALF OF INVARIANT 1 — the map must be able to tell a backed cvar from an unbacked one, not
	 * merely answer without crashing. See FPMSaveGuardUserSettingMapIsAlive() above: this is the same
	 * known-positive/known-negative proof FPMResidueSentinel.cpp already runs, applied to the map this
	 * file's before-hook filter actually calls. Fail CLOSED exactly like a failed hook install: a guard
	 * that cannot discriminate is a guard that stands nothing down, and arming it anyway is how a held
	 * US_*-backed cvar reaches GameUserSettings.ini while every other log line reads clean.
	 */
	if (!FPMSaveGuardUserSettingMapIsAlive())
	{
		Fail(TEXT("the user-setting map's IsBacked() classifier failed its known-positive/known-negative "
		          "proof (see the error above). Arming would report healthy while the before-hook's "
		          "filter stands nothing down before every save."));
		return;
	}

	bGFPMSaveGuardArmed = true;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] save-settings guard ARMED on UFGGameUserSettings::SaveSettings (both entry and exit). "
		     "Self-test: both hooks installed, user-setting map reachable AND discriminating (source: %s). "
		     "It stands FPM's holds on capturable cvars down across the game's save and puts them back "
		     "after, so a transient write can never become a permanent setting. NOT yet proven end-to-end - that "
		     "needs a boot with a real save. Clause 6 STOPS refusing the US_*-backed set right at this "
		     "line: IsHealthy() is already true here (armed, not failed, nothing suspended), so a "
		     "US_*-backed write is allowed from this point on, before the save-and-restore path has "
		     "ever run for real."),
		FPMUserSettingMap::Source() == FPMUserSettingMap::ESource::RuntimePlusTables
			? TEXT("runtime + tables") : TEXT("tables only"));
}

void FFPMSaveSettingsInterceptor::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe: both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps
	 * alike, uninstalling the detour once both are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (SaveBeforeHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGGameUserSettings::SaveSettings, SaveBeforeHandle);
		SaveBeforeHandle.Reset();
	}
	SaveAfterHandle.Reset();   // same method as above - that call cleared both maps
}

void FFPMSaveSettingsInterceptor::LogReport(FOutputDevice* Ar)
{
	int32 SavesSeen = 0, HoldsSuspended = 0;
	GetCounts(SavesSeen, HoldsSuspended);

	const FString Line = FString::Printf(
		TEXT("[FPM] save guard: %d save(s) seen · %d hold(s) suspended across them. A guard that has "
		     "never fired should be visibly a guard that has never fired."),
		SavesSeen, HoldsSuspended);

	if (Ar != nullptr)
	{
		Ar->Log(Line);
	}
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
}

/*
 * `FPM.SaveGuard.Report` — takes the output device so it prints in the console she is looking at as
 * well as the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command
 * that answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMSaveGuardReportCmd(
	TEXT("FPM.SaveGuard.Report"),
	TEXT("Print how many saves this guard has seen, and how many FPM cvar holds it suspended across "
	     "them, this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMSaveSettingsInterceptor::LogReport(&Ar);
	}));
