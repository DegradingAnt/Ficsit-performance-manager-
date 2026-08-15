// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ TWO ONE-BOOT READS FOR THE B9 / B11 QUESTIONS IN FPM2-DESIGN-ASSEMBLED.md SECTION 17.
 *
 * Neither of these fixes anything, hooks anything, or writes anything — they are read-only probes,
 * same family as `FPM.D0` / `FPM.Support` / `FPM.CVars`. That is why this is a plain class with static
 * functions and NOT an `IFPMFix`: there is no hook to arm and no fix to disarm, and routing a
 * do-nothing-but-read command through the fix ledger would be the "unarmed fix" shape
 * `check_structure.py` exists to catch, for a class that was never a fix to begin with.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B9 — `FGTimeOfDaySubsystem`'s non-cheat pin API.
 *
 * [MEASURED 2026-08-15, direct read of `FGTimeSubsystem.h`, the game's own shipped header]: the answer
 * is already YES from source, no boot required for the EXISTENCE half. `AFGTimeOfDaySubsystem::
 * SetDaySeconds(float)` (`FGTimeSubsystem.h:48`) and `SetTimeSpeedMultiplier(float)` (`:128`) are both
 * plain `public:` members — not `UFUNCTION(exec, CheatBoard, ...)` like `UFGCheatManager`'s equivalents,
 * not gated behind `EnableCheats` at all. `SetDaySeconds`'s own doc comment says "most useful for
 * editor preview", which is exactly a pin use case, and native FPM code can call either once it holds
 * a valid `AFGTimeOfDaySubsystem*` from `Get(UWorld*)` (`:61`).
 *
 * `FPM.Diag.TimeOfDay` proves REACHABILITY (a valid subsystem instance exists at runtime, its read
 * accessors resolve) without calling either setter — flipping the day/night cycle from a diagnostic
 * command has a real player-visible side effect and this probe is not the place to spend that.
 * Confirming the PIN behaviour itself (call `SetDaySeconds`, observe the clock hold) is left as a
 * one-line follow-up for whoever builds the M-DAWN-style lever, not for this read-only command.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B11 — vanilla player skeleton sockets, forearm/hand, both sides.
 *
 * [MEASURED 2026-08-15, direct read of `FPMWristItemBase.h:118,121`]: the wrist-slot system ALREADY
 * ships a concrete guess — `mSocketLeft = "hand_lSocket"`, `mSocketRight = "hand_rSocket"` — and the
 * header itself says so: "B11 has not measured the real vanilla socket names against the shipped
 * skeleton, so mSocketLeft/mSocketRight's DEFAULTS are placeholders." So the useful probe is not a
 * blind dump of every socket name; it is a targeted PASS/FAIL against the two names the shipped code
 * already depends on, on both the first-person mesh (`GetMesh1P()`) and the third-person mesh
 * (`GetMesh3P()`) — printed beside the full socket list so an operator can read off the real name if
 * the guess is wrong, in the same command, without a second boot.
 *
 * `FPM.Diag.Sockets` requires a spawned local player character to answer anything; run it after
 * loading into a world, never from the main menu. Coverage is stated explicitly when that precondition
 * fails, per this project's own "an instrument must print its own coverage" rule.
 */
class FPMBootProbes
{
public:
	/** `FPM.Diag.TimeOfDay` — read the day/night subsystem's current state and report the pin API's
	 *  reachability. Writes nothing; calls no setter. */
	static void ReportTimeOfDay(class UWorld* World, class FOutputDevice* Ar);

	/** `FPM.Diag.Sockets` — enumerate socket names on the local player's first- and third-person mesh,
	 *  and check the two names FPMWristItemBase already assumes on each. */
	static void ReportSockets(class UWorld* World, class FOutputDevice* Ar);
};
