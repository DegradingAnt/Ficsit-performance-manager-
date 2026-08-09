// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMResidueSentinel.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMBoxCache.h"
#include "Core/FPMCVarWriter.h"
#include "UI/FPMChatRelay.h"   // the drill's verdict goes to chat as well as the log (Ant, 2026-08-09)

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

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

	/**
	 * ★ THE CLASSIFIER'S LIVENESS PROOF — Ant, 2026-08-09: *"dead instruments are not my preferred item
	 * to exist."*
	 *
	 * The audit's US_* check can never fire in normal operation, because clause 6 REFUSES the writes that
	 * would trip it. That is correct behaviour and it leaves the detector unproven — the exact shape that
	 * has now cost this project six incidents. The real failure path cannot be staged safely (the sentinel
	 * must not leak on purpose), so the CLASSIFIER is proven instead: a known-positive must return true
	 * and a known-negative must return false. If either stops holding, the audit has gone blind and says
	 * so, rather than reporting a confident clean sheet forever.
	 */
	bool FPMSentinelClassifierIsAlive()
	{
		const bool bPositive = FPMCVarWriter::IsUserSettingBacked(TEXT("t.MaxFPS"));
		const bool bNegative = FPMCVarWriter::IsUserSettingBacked(TEXT("FPM.SelfTest.Probe"));
		if (bPositive && !bNegative) { return true; }

		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ THE RESIDUE CLASSIFIER IS DEAD: 't.MaxFPS' classified %s (expected US_*-backed) "
			     "and 'FPM.SelfTest.Probe' classified %s (expected NOT backed). Every 'nothing would remain' "
			     "below is worthless until this is fixed."),
			bPositive ? TEXT("backed") : TEXT("NOT backed"),
			bNegative ? TEXT("BACKED") : TEXT("not backed"));
		return false;
	}
}

int32 FPMResidueSentinel::Audit()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- residue audit: if settings saved now and the mod were deleted, what remains? ----"));

	const bool bClassifierAlive = FPMSentinelClassifierIsAlive();

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

	/*
	 * INPUT 4 — FILES. ⚠ ADDED 2026-08-09 AFTER THIS AUDIT MISSED 120,681 BYTES OF ITS OWN MOD'S RESIDUE.
	 *
	 * The audit inspected cvars only, so a file written outside the plugin was a category it could not
	 * see — and it reported "NOTHING would remain" while `DerivedBoxes.json` sat in the game's Saved
	 * directory surviving every uninstall. A residue auditor blind to files is not a partial auditor; on
	 * the question actually being asked it is a wrong one.
	 */
	const FString CachePath = FPMBoxCache::GetCacheFilePath();
	const bool bCacheExists = IFileManager::Get().FileExists(*CachePath);
	FString PluginDir;
	if (const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager")))
	{
		PluginDir = FPaths::ConvertRelativePathToFull(Self->GetBaseDir());
	}
	/*
	 * ⚠ THE TRAILING SEPARATOR IS LOAD-BEARING — third-pass finding, 2026-08-09.
	 *
	 * Without it, `StartsWith` is a plain string prefix test, so a SIBLING directory named
	 * `FicsitsPerformanceManagerAnything` would match and its file would be reported as "inside the
	 * plugin, not residue". No such directory exists today, so this is latent rather than live — but a
	 * false NOT-RESIDUE verdict is the one error a residue checker must never make, and it would be
	 * completely silent.
	 *
	 * Case-insensitive is correct here and deliberate: `StartsWith` defaults to `ESearchCase::IgnoreCase`
	 * and this is a Windows path comparison.
	 */
	if (!PluginDir.IsEmpty() && !PluginDir.EndsWith(TEXT("/")))
	{
		PluginDir += TEXT("/");
	}
	const bool bInsidePlugin = !PluginDir.IsEmpty()
		&& FPaths::ConvertRelativePathToFull(CachePath).StartsWith(PluginDir);

	int32 FileResidue = 0;
	if (!bCacheExists)
	{
		// Stated, not silent. "We looked and there is no file" and "nobody checked files" must not produce
		// the same output -- that equivalence is the whole reason this input was missing in the first place.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   file: no cache on disk yet (%s)."), *CachePath);
	}
	else if (bInsidePlugin)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   file: %s — INSIDE the plugin, so it is deleted with the mod. Not residue."),
			*CachePath);
	}
	else
	{
		/*
		 * ⚠ THIS IS A DECLARED EXCEPTION, AND IT IS COUNTED AS ONE — corrected on a second pass over my own
		 * fix, 2026-08-09. The first version LOGGED the words "the declared fallback" while the exception
		 * table held zero entries, so the audit simultaneously claimed the file was sanctioned and counted
		 * it as unsanctioned residue. Prose is not a declaration; the table is. Ant's ruling was "keep it in
		 * the mod if possible, if its not we declare it an exception" — so when the fallback is in force it
		 * belongs in the count of DECLARED things, not in the count of surprises.
		 */
		++FileResidue;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   file: %s — OUTSIDE the plugin, so it SURVIVES uninstall. DECLARED EXCEPTION: the "
			     "plugin directory was not writable, so the cache fell back here. Derived data only, and "
			     "safe to delete by hand at any time."),
			*CachePath);
	}

	/*
	 * ⚠ THE TWO KINDS OF RESIDUE GET DIFFERENT REMEDIES, so the verdict must not merge them — second-pass
	 * correction. The first version printed "find the write path that bypassed clause 6" for ANY non-zero,
	 * which for a FILE would have sent the reader hunting a cvar bug that does not exist. A wrong remedy in
	 * a diagnostic costs more than no remedy.
	 */
	if (WouldRemain == 0 && FileResidue == 0 && bClassifierAlive)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   RESULT: NOTHING would remain. FPM holds only transient console values at 0x07, "
			     "released through the engine's own tagged history, and writes no file outside its own "
			     "plugin folder. ⚠ This says nothing about a leak from an EARLIER build - a value already "
			     "written into GameUserSettings.ini is now the player's own setting and is indistinguishable "
			     "from one they chose."));
	}
	else
	{
		UE_CLOG(WouldRemain > 0, LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   RESULT: %d UNDECLARED cvar hold(s) would survive. This is not acceptable - a "
			     "US_*-backed cvar is being held, so find the write path that bypassed clause 6."),
			WouldRemain);
		UE_CLOG(FileResidue > 0, LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   RESULT: %d declared file exception(s) would survive. Expected when the plugin "
			     "directory is read-only; it is derived data and deleting it by hand is always safe."),
			FileResidue);
		UE_CLOG(!bClassifierAlive, LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   RESULT: UNKNOWN - the classifier is dead, so no verdict above can be trusted."));
	}

	// ⚠ RETURNS ONLY THE UNDECLARED cvar count. A DECLARED file exception is not a failure, and folding it
	// in would make the residue drill fail permanently on any read-only install while blaming the release
	// path — which was exactly the defect this second pass found.
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

	/*
	 * ★ ReleaseOwner, NOT ReleaseAll — review finding, 2026-08-09, and it would have been a live defect.
	 *
	 * `ReleaseAll` drops EVERY hold in the process. This is a console command a human can run at any
	 * moment, so once the governor exists, running a diagnostic would silently tear down live levers and
	 * the player would see the mod stop working for no visible reason. Releasing only what the drill
	 * itself took proves exactly the same thing about the release path. The all-or-nothing OFF switch
	 * still has its own command, `FPM.Off`, where that IS the intent.
	 */
	FPMCVarWriter::Get().ReleaseOwner(Owner);

	const FString After = Var->GetString();
	const EConsoleVariableFlags SetByAfter =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);
	const int32 AfterRelease = Audit();

	/*
	 * The value AND the SetBy must both come back. A drill that only checked the value would pass while our
	 * priority tag still sat on the variable locking out every lower writer — residue invisible to anyone
	 * reading the number.
	 *
	 * ⚠ `Audit() == 0` IS THE RIGHT BAR **ONLY BECAUSE** Audit returns the UNDECLARED cvar count and not the
	 * declared file exception. Fold the file count into that return and this drill fails permanently on
	 * every read-only install — while its message blames the release path, which would be working perfectly.
	 * That was a real defect in the first version of this fix, caught on a second pass over it; it is why
	 * the return value is deliberately narrow. Do not widen it without changing this line too.
	 *
	 * Context worth keeping: the UE convention for "uninstall a plugin cleanly" is that the USER deletes
	 * Saved/ and Intermediate/ by hand. There is no engine facility that audits what a plugin leaves
	 * behind, which is why this exists at all rather than wrapping something.
	 */
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

	/*
	 * ★ AND TO CHAT. Ant's standing rule, stated twice on 2026-08-09: "we really should print stuff to
	 * the chat instead (that is important of course) along with the log. a player wont check logs for
	 * stuff, only devs do" — and again of this exact command, "log and chat then".
	 *
	 * THE VERDICT ONLY, NOT THE AUDIT. The full run writes roughly a dozen lines: the two audits, every
	 * held cvar, the file rows. That belongs in the log. Ant, of an earlier chat surface: "print what is
	 * relevant, not the entire log" — and the relay's own flood cap is 12 lines per 5 seconds, so dumping
	 * the whole drill would trip it and mute the very line worth reading.
	 *
	 * WHY THIS COMMAND EARNED IT. It ran twice on the 0.6.0 boot and PASSED both times, and Ant saw
	 * nothing at all: the console does not echo Display-level logs, so the only evidence was in
	 * FactoryGame.log. A guard whose result is invisible to the person running it reads as broken, and I
	 * reported it as broken twice before checking the file. One line here removes that whole failure mode.
	 */
	if (bPass)
	{
		FPMChatf(TEXT("[FPM] residue drill PASSED. Nothing would be left behind. (Weak evidence at this "
		              "stage - little is registered yet.)"));
	}
	else
	{
		// A failure is exactly what a player must not have to go log-diving to discover.
		FPMChatf(TEXT("[FPM] residue drill FAILED - FPM could leave settings behind on this build. "
		              "Details in FactoryGame.log."));
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
