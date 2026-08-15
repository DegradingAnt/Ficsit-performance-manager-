// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ FPM.Verify — ONE COMMAND, EVERY SELF-TEST THIS BUILD ALREADY OWNS.
 *
 * A plain read-only class, same reason `FPMBootProbes` is one: nothing here hooks anything, arms
 * anything or writes a cvar, so there is no fix to disarm and routing it through `IFPMFix` would be
 * the "unarmed fix" shape `check_structure.py` exists to catch for a class that was never a fix.
 *
 * WHAT THIS ANSWERS. Seven subsystems already carry their own `SelfTest()`, each proven the way this
 * project's own build discipline demands — round-trip a known-positive and a known-negative through
 * the REAL code path, never an assertion that it "should" work:
 *
 *   1. `FPMCVarWriter::Get().SelfTest()`      — the write path, on FPM's own scratch cvar.
 *   2. `FPMFixes::SelfTest()`                 — the fix registry's own arm/side-gate invariants.
 *   3. `FFPMDetectorRegistry::SelfTest()`     — one known probe entry, round-tripped through the store.
 *   4. `FFPMLeverRegistry::Get().SelfTest()`  — the lever registry's fixture levers.
 *   5. `FFPMStageTables::Get().SelfTest()`    — the stage ladder's own fixture rows.
 *   6. `FFPMGiveTakeWalk::Get().SelfTest()`   — the give/take walk's fixture ladder.
 *   7. `FFPMHostTier::SelfTest(Ar)`           — `FPMClassifyHostTier` against known cases.
 *
 * Each was already reachable one at a time, from seven different console commands, on seven different
 * boots. `FPM.Verify` is not new coverage — it is the same seven calls, in one place, printed as one
 * PASS/FAIL/UNREACHABLE table with a coverage ratio, so a single boot can answer "did everything that
 * already claims to self-test actually pass, right now" without Ant typing seven commands and
 * cross-referencing seven logs by hand.
 *
 * ★ WHY FIVE OF THE SEVEN CHECK `FPMFixes::IsArmed` FIRST, AND DO NOT CALL `SelfTest()` WHEN IT IS
 * FALSE. Five of the seven subsystems (`FFPMDetectorRegistry`, `FFPMLeverRegistry`, `FFPMStageTables`,
 * `FFPMGiveTakeWalk`, `FFPMHostTier`) are `IFPMFix` classes whose fixture rows are registered inside
 * their own `Arm()` — never inside `SelfTest()` itself (see each class's own `.cpp`: `Arm()` calls
 * `RegisterSelfTestLevers()` / equivalent, THEN `SelfTest()`). A fix that is not armed this session
 * (side-gated off, or toggled off through `FPM.Fix.*`) has never populated those fixtures, and calling
 * `SelfTest()` against an empty registry is not a verified code path — this file does not guess what
 * it would do. `FFPMGiveTakeWalk` in particular is `Side() == NeverOnDedicatedServer`, so on a
 * dedicated server this line reports UNREACHABLE honestly instead of a FAIL that would misdescribe a
 * side-gate as a defect. `FPMCVarWriter` and `FPMFixes` are always-on core subsystems with no arm
 * concept of their own, so those two are always called.
 *
 * ★ NEVER PRINTS A MISSING SUBSYSTEM AS A PASS. UNREACHABLE is its own outcome, distinct from PASS and
 * FAIL, and the coverage ratio counts only what was actually reachable this session — a subsystem this
 * build cannot currently exercise is not silently dropped from the total, and it is never counted
 * toward a clean sheet.
 *
 * ★ THE KNOWN-NEGATIVE CONTROL. Same doctrine `FPMLeverRegistry::SelfTest`'s known-negative case and
 * `FFPMHostTier::SelfTest`'s known-negative branch already use, turned on the COMMAND ITSELF: after
 * the seven real subsystems are printed, `Run()` drives one more check that is STRUCTURALLY certain to
 * report FAIL — it asks `IConsoleManager` for a console-variable name that has never been registered by
 * anything, anywhere, and reports PASS only if that lookup somehow succeeds. If it ever does report
 * PASS, that is not a subsystem defect, it is proof this command's own PASS/FAIL classifier is
 * inverted, and `Run()` treats that as a hard self-check failure rather than one more row in the table
 * — a false clean sheet is impossible without the control itself lying, and the control has nothing
 * left to lie about.
 */
class FPMBootVerify
{
public:
	/** `FPM.Verify` — run every subsystem self-test this build owns, print PASS/FAIL/UNREACHABLE per
	 *  subsystem plus a coverage ratio, then prove the classifier itself with a known-negative control.
	 *  Read-only: writes no cvar, arms nothing, disarms nothing. */
	static void Run(class FOutputDevice* Ar);
};
