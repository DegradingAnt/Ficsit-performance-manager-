// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMResidueSentinel.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * ★ THE NAMED-EXCEPTION TABLE — the ONLY sanctioned durable surface, and it is EMPTY.
	 *
	 * An empty table is a real state, not an unfinished one: FPM2 writes no ini, no save and no
	 * registry, so the correct expected residue is nothing at all. The table exists as a declared,
	 * enumerable place so that the day something legitimately needs to persist, it arrives WITH Ant's
	 * recorded ruling, a boot-log mention, a README line and a row here — rather than as a quiet
	 * `GConfig->SetString` somewhere in a fix.
	 *
	 * ⚠ Name prefixed for the UNITY BUILD (FPMFixContract.h:166-171).
	 */
	const TCHAR* const GFPMSentinelNamedExceptions[] = { nullptr };
	constexpr int32 GFPMSentinelNumExceptions = 0;
}

int32 FPMResidueSentinel::Audit()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- residue audit: if settings saved now and the mod were deleted, what remains? ----"));

	// INPUT 1 — the named-exception table. Enumerated even when empty, because "we checked and there are
	// none" and "nobody looked" produce identical silence otherwise.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   named exceptions (the only sanctioned durable surface): %d"),
		GFPMSentinelNumExceptions);
	for (int32 i = 0; i < GFPMSentinelNumExceptions; ++i)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]     EXPECTED to persist: %s"), GFPMSentinelNamedExceptions[i]);
	}

	// INPUT 2 — the writer's ledger. This is the whole of what FPM currently holds; anything held through
	// another path is invisible here, which is exactly why the writer is the single path.
	FPMCVarWriter::Get().LogLedger();

	/*
	 * INPUT 3 — would any held cvar be CAPTURED by the game's own save?
	 *
	 * The writer already refuses the US_*-backed set outright (clause 6), so the expected count is zero
	 * and this loop is the check that the refusal actually held. It is not redundant with the refusal:
	 * the denylist is known-incomplete by 242 entries, and a lever added through some future path that
	 * forgot the writer would never have hit the refusal at all.
	 */
	/*
	 * ⚠ THE FIRST DRAFT OF THIS FUNCTION COULD NOT FAIL, and that is worth recording rather than quietly
	 * fixing. It set `WouldRemain = 0` and never iterated anything, because the ledger was private to the
	 * writer — so it would have printed a confident "NOTHING would remain" for the rest of the mod's life
	 * regardless of what FPM actually held. An auditor whose result is structurally constant is not a weak
	 * auditor, it is a decoration. It is the same defect class this project has now hit five times, and
	 * this one was mine, in the file whose entire job is to catch it.
	 */
	TArray<FString> Held;
	FPMCVarWriter::Get().GetHeldCVars(Held);

	int32 WouldRemain = 0;
	for (const FString& CVar : Held)
	{
		if (!FPMCVarWriter::IsUserSettingBacked(*CVar)) { continue; }

		++WouldRemain;
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]     ⚠ WOULD SURVIVE: '%s' is US_*-backed and FPM is holding it. The game's next "
			     "settings save writes it into the player's own file, where it outlives uninstall. "
			     "Clause 6 should have refused this write - find the path that bypassed the writer."),
			*CVar);
	}

	if (WouldRemain == 0 && GFPMSentinelNumExceptions == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   RESULT: NOTHING would remain. FPM holds only transient console values at 0x07, "
			     "released through the engine's own tagged history. ⚠ This says nothing about a leak from "
			     "an EARLIER build - a value already written into GameUserSettings.ini is now the player's "
			     "own setting and is indistinguishable from one they chose."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   RESULT: %d hold(s) would survive beyond the %d declared exception(s). "
			     "That is residue and it is not acceptable - find the write path that bypassed clause 6."),
			WouldRemain, GFPMSentinelNumExceptions);
	}

	return WouldRemain;
}

bool FPMResidueSentinel::Drill()
{
	static const FName Owner(TEXT("residue-drill"));
	const TCHAR* const Probe = TEXT("FPM.SelfTest.Probe");

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- residue drill ----"));

	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Probe);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] residue drill CANNOT RUN: '%s' is missing. Not a pass and not a fail - unrun."),
			Probe);
		return false;
	}

	const FString Before = Var->GetString();
	const EConsoleVariableFlags SetByBefore =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	FPMCVarWriter::Get().Hold(Owner, Probe, TEXT("7777"), TEXT("residue drill: acquire"));
	const int32 DuringHeld = Audit();

	FPMCVarWriter::Get().ReleaseAll(TEXT("residue drill: the OFF switch under test"));

	const FString After = Var->GetString();
	const EConsoleVariableFlags SetByAfter =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);
	const int32 AfterRelease = Audit();

	// The value AND the SetBy must both come back. A drill that only checked the value would pass while
	// our priority tag sat on the variable locking out every lower writer -- residue that is invisible
	// to anyone reading the number.
	const bool bPass = (After == Before) && (SetByAfter == SetByBefore)
	                && (DuringHeld == 0) && (AfterRelease == 0);

	if (bPass)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] residue drill PASSED: %s returned to '%s' at %s, audit clean before and after. "
			     "⚠ READ THIS AS WEAK EVIDENCE TODAY - almost nothing is registered yet, so of course "
			     "nothing leaked. It becomes a real gate at P5 when the ladder holds a population."),
			Probe, *After, GetConsoleVariableSetByName(SetByAfter));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] residue drill FAILED: value '%s' -> '%s' (expected '%s'), SetBy %s -> %s, "
			     "audit %d held / %d after release. FPM is capable of leaving residue on this build."),
			*Before, *After, *Before,
			GetConsoleVariableSetByName(SetByBefore), GetConsoleVariableSetByName(SetByAfter),
			DuringHeld, AfterRelease);
	}
	return bPass;
}

static FAutoConsoleCommand GResidueAuditCmd(
	TEXT("FPM.Residue"),
	TEXT("Audit what would remain on this machine if settings saved now and FPM were deleted."),
	FConsoleCommandDelegate::CreateStatic([]() { FPMResidueSentinel::Audit(); }));

static FAutoConsoleCommand GResidueDrillCmd(
	TEXT("FPM.ResidueDrill"),
	TEXT("Run the residue drill: hold, audit, release, audit, and prove the machine is back where it started."),
	FConsoleCommandDelegate::CreateStatic([]() { FPMResidueSentinel::Drill(); }));
