// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ THE RESIDUE AUDITOR — design §2.5, built at P1.4. READ-ONLY by construction.
 *
 * It answers one question, at any moment, in one command:
 *
 *     "If the game saved its settings right now and this mod were then DELETED,
 *      what would remain on the player's machine?"
 *
 * The expected answer is: **exactly the named-exception keys, and nothing else.** Today FPM2 has no
 * named exceptions at all — it writes no ini, no save, no registry — so the expected answer is
 * literally nothing, and the sentinel's job is to CHECK that rather than to assert it.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * WHY AN AUDITOR AT ALL, WHEN THE WRITER ALREADY REFUSES THE DANGEROUS SET
 *
 * Because "the writer refuses it" is a claim about code, and residue is a fact about a machine. The
 * two can diverge in ways that are individually plausible and collectively fatal: a denylist that is
 * known-incomplete (242 of 272 US_* assets are UNMAPPED), a future lever added through a path that
 * forgot the writer, a game update that moves a cvar into the user-settings set. Every one of those
 * looks fine at the write site. This is the thing that looks at the ledger instead.
 *
 * ⚠ WHAT IT CANNOT DO, STATED SO NOBODY READS A PASS AS MORE THAN IT IS:
 *
 * 1. **A leak from a PAST session is invisible to it, by construction.** It audits what FPM holds NOW.
 *    If an earlier build already wrote a value into `GameUserSettings.ini`, that value is now the
 *    player's own setting and nothing here can tell it apart from one they chose. The design's answer
 *    is prevention going forward plus one documented restore with Ant — **no retroactive scanner is
 *    built, deliberately**, because a scanner that guesses which of a player's settings were ours is
 *    a worse failure than the leak.
 * 2. **At P1 a PASS is near-trivial and must be read that way.** Almost nothing is registered yet, so
 *    of course nothing leaks. The drill earns its meaning at P5, when the ladder holds a real
 *    population of cvars, and it re-runs there as a gate. Saying so here is the point: a green light
 *    whose weakness is undocumented is how a release gate becomes a formality.
 */
class FICSITSPERFORMANCEMANAGER_API FPMResidueSentinel
{
public:
	/**
	 * The audit. Prints every hold that WOULD survive, and why. Bound to `FPM.Residue`.
	 *
	 * @return the number of holds that would leave residue. Zero is the only acceptable answer.
	 */
	static int32 Audit();

	/**
	 * THE DRILL — the in-mod half of the acceptance cycle that has never run.
	 *
	 * Hold something, audit, release, audit again, and assert the machine is back where it started.
	 * It uses FPM's own probe cvar, never a game cvar: a drill that could itself leave residue would
	 * be the joke that writes itself.
	 *
	 * Bound to `FPM.ResidueDrill`. It is a RELEASE GATE at P5 and P7, not a developer convenience.
	 */
	static bool Drill();
};
