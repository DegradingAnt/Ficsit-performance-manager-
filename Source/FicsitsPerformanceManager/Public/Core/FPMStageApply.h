// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMGiveTake.h"
#include "Core/FPMStageTables.h"

/**
 * ★ SLICE 2 -- THE APPLY PASS. The thing that turns a decision into a real console-variable write.
 *
 * Design section 9.1 (FPM2-DESIGN-ASSEMBLED.md:1250-1256): every lever drives "through FPMCVarWriter
 * (section 5.1), batched into ONE apply pass per decision (every settings change hitches -- batching
 * is a law)".
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★★ WHAT THIS FILE CLOSES, IN THE MOD'S OWN WORDS
 *
 * Until this file existed the boot log said, every session:
 *     "stage tables armed: N lever(s) accepted ... NOTHING IS APPLIED FROM HERE"
 *     "give/take walk armed. NOTHING DRIVES IT YET ... the apply pass that would execute a
 *      decision is not built."
 * The tables held real content, the walk made real decisions, and no code path could move a single
 * cvar. This is that code path. Both of those log lines were edited when it landed, because a log
 * line that outlives its own condition is how a refusal outlived the interceptor it was waiting for
 * (FPMCVarWriter.cpp's clause-6 note records that exact failure).
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE BASELINE PROBLEM, AND WHY IT IS NOT A BLOCKER ANY MORE
 *
 * FPMGiveTake.h:56-64 named the reason the apply pass was deferred: policy arithmetic
 * (MaxOf/MinOf/BaseScale/BaseDelta) needs a BASELINE, Law 3 wants it from a shipped vanilla-defaults
 * table, and no such table exists for engine r.* cvars. Building one would be an invention;
 * re-reading the live cvar is the exact RATCHET Law 3 forbids.
 *
 * The third option was already built and already declared. FFPMLeverRegistry::CaptureBaselineOnce
 * captures from the live cvar EXACTLY ONCE, and REFUSES if FPMCVarWriter already holds it -- so the
 * capture can only ever happen before FPM's first write, which is precisely the value Law 3 wants.
 * Every stage lever with a baseline-comparing policy already declares BaselineSource=CapturedOnce
 * (FPMStageTables.cpp, RegisterOne). This file is the first caller of that machinery.
 *
 * ⚠ THE RATCHET IS CLOSED BY THE "ONCE", NOT BY THE ORDER OF CALLS. Engage K1, release it, engage it
 * again: the second engage projects from the value captured before the FIRST hold, because the
 * capture is idempotent and never re-reads. A live-read implementation would compound -- BaseScale
 * would multiply twice -- and the self-test proves this one does not, by applying a scaling lever
 * twice and requiring the same landing both times AND requiring that a live-read implementation
 * would have produced a DIFFERENT number (otherwise the check proves nothing and says so).
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ ZERO RESIDUE: RELEASE IS AN ENGINE UNSET, NOT A WRITE-BACK
 *
 * Nothing here captures a value in order to restore it. A release is FPMCVarWriter::Release, which
 * is Var->Unset(0x07, "FPM") -- the engine drops our entry from its own priority history and the
 * value underneath reappears. So a release cannot restore the WRONG value, because it does not
 * restore anything; it removes us. FPM.Off, module shutdown, a new world and this fix's Disarm all
 * route here, and each of those is wired rather than promised (see Arm()).
 *
 * ⚠ MODULE LEASE, NOT WORLD LEASE, AND THAT IS A DELIBERATE CHOICE. EFPMLease::World reads like the
 * right answer and its rationale in FPMCVarWriter.h is sound, but NOTHING in this tree releases a
 * lease when a world ends -- grep for EFPMLease::World: this would have been its first use, and no
 * teardown path keys on it. Declaring a lease nobody honours would leave gameplay values held in the
 * menu world, which is the very failure that comment describes. So the holds are Module-leased and
 * this fix releases them itself at OnWorldLoad, at Disarm, and on the master switch's OFF path.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ A GROUP STEP WRITES MEMBERS, NEVER THE GROUP
 *
 * FPMCVarWriter clause 2 refuses every sg.* write unconditionally, so a ScalabilityGroup lever moves
 * its group by writing the values the TARGET TIER prescribes to the group's member cvars, read from
 * the live BaseScalability.ini through the registry's alias table. Two consequences worth stating:
 *   - Every member passes FPMClassifyGroupMember first, which is the ONE site that knows the GI kill
 *     switch and the US_*-backed set are unreachable. The exclusions are COUNTED and printed,
 *     because a working filter and a dead alias table produce the same clean result.
 *   - Because sg.* is never written, the group cvar keeps reporting the PLAYER'S tier. Re-applying
 *     the same tier therefore resolves to the same target and writes the same values. The group path
 *     cannot ratchet for the same reason it cannot be written.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ⚠ WHAT THIS FILE DOES NOT DO, STATED SO NOBODY READS MORE INTO IT
 *
 * It does not DECIDE (FPMGiveTake), it does not carry the resolution lever (section 8 -- a
 * ResolutionDown/Up decision is reported as NOT EXECUTED HERE, with that reason, rather than
 * silently succeeding), it does not touch CPU relief (section 3.7, a separate lever on a separate
 * signal), and it does not drive itself: no timer in this file calls Execute. The governor tick that
 * would is named in the report, along with what it is still waiting for.
 */

/** What happened to ONE lever in an apply pass. Every value is a state a reader can act on. */
enum class EFPMApplyOutcome : uint8
{
	/** Held at its target through FPMCVarWriter. */
	Written,

	/** Our hold was dropped; the engine's own history restored whatever was underneath. */
	Released,

	/** The registry has no entry for this lever -- it was REFUSED at registration (Law 1 / clause 2),
	 *  so the tier is genuinely smaller than the design's table and says so. */
	SkippedNotRegistered,

	/** The capability probe said ABSENT, or the probe pass has not run and availability is Unknown.
	 *  Unknown is NOT treated as available: guessing is how a 15GB tier reaches an 8GB card. */
	SkippedUnavailable,

	/** A baseline-comparing policy whose baseline could not be captured safely (CaptureBaselineOnce
	 *  refused, or the captured text is not a number). Refusing to write is the correct outcome:
	 *  the alternative is inventing a baseline. */
	SkippedNoBaseline,

	/** ResolveGroupTarget refused the step. TWO cases reach this, and the line's Why says which: the
	 *  GI FLOOR (the design's own wording, "reports 'at GI floor, skipped' and the walk proceeds to
	 *  the next lever") and the group CEILING, where a +1 from the top tier would land outside the
	 *  alias table. It is not named after the floor alone, because an outcome label that says "floor"
	 *  while printing a ceiling refusal is a small lie in the one place a reader trusts. */
	SkippedGroupStepRefused,

	/** A group member FPMClassifyGroupMember excluded (the GI kill switch, or a US_*-backed cvar). */
	SkippedExcludedMember,

	/** FPMCVarWriter refused the write and logged why. Counted here so a tier that wrote nothing is
	 *  never reported as a tier that had nothing to write. */
	WriterRefused,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMApplyOutcome Outcome);

/** ONE LINE OF AN APPLY PASS, for the report and for the self-test's assertions. */
struct FICSITSPERFORMANCEMANAGER_API FFPMApplyLine
{
	/** The cvar actually addressed. For a group step this is a MEMBER, not the group. */
	FString CVarName;

	/** Non-empty when this line came from expanding a scalability group. */
	FString FromGroup;

	EFPMApplyOutcome Outcome = EFPMApplyOutcome::SkippedNotRegistered;

	/** What the cvar read before we touched it, for the reader of a support dump.
	 *  ⚠ NEVER used as a baseline. The baseline comes from CaptureBaselineOnce and from nowhere
	 *  else; this field exists to be printed. */
	FString ObservedBefore;

	/** The value we asked for. Empty on a release, where we ask for nothing. */
	FString Target;

	/** Free text: the projection note, the clamp, the exclusion reason, the refusal. Always set. */
	FString Why;
};

/**
 * THE RESULT OF ONE APPLY PASS. Counts and lines, never a bare bool -- "it worked" and "it had
 * nothing to do" are different facts and a caller has to be able to tell them apart.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMApplyResult
{
	EFPMStageTier   Tier   = EFPMStageTier::None;
	EFPMSteerAction Action = EFPMSteerAction::None;

	/** False when the pass was refused as a whole (master switch off, unknown tier, a decision this
	 *  file does not execute). Refusal names itself. */
	bool bExecuted = false;
	FString Refusal;

	int32 Written  = 0;
	int32 Released = 0;
	int32 Skipped  = 0;

	/** ★ COVERAGE FOR THE GROUP FILTER. How many members FPMClassifyGroupMember dropped, split by
	 *  reason. Zero is legitimate and must still be PRINTED: a filter that fired and an alias table
	 *  that expanded to nothing both look like a clean pass from the outside. */
	int32 ExcludedGIKillSwitch = 0;
	int32 ExcludedUserSetting  = 0;

	/** How many alias members the group step examined. The denominator for the two counts above. */
	int32 GroupMembersExamined = 0;

	TArray<FFPMApplyLine> Lines;

	/** True when the pass changed the machine in some way. */
	bool ChangedAnything() const { return Written > 0 || Released > 0; }
};

class FICSITSPERFORMANCEMANAGER_API FFPMStageApply final : public IFPMFix
{
public:
	static FFPMStageApply& Get();

	virtual const TCHAR* Name() const override { return TEXT("stage-apply"); }

	/** GPU-side quality levers, so the same answer the tables and the walk both give. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** UnknownCause, the answer FPMLeverRegistry.h:55-58 and FPMStageTables.h give for the same
	 *  reason: nothing here is being FIXED. It is a write path plus the proofs that keep it honest. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Steering; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/**
	 * ★ EXECUTE ONE DECISION. The whole point of the file.
	 *
	 * EngageCut / PromoteBonus  -> hold every lever the tier carries, in the table's own order.
	 * ReleaseCut / DemoteBonus  -> drop every hold this tier owns; the engine restores what was under.
	 * ResolutionDown / Up       -> NOT EXECUTED HERE. Section 8 owns the resolution lever and it is a
	 *                              Slice-3 investigation. The result says so in Refusal rather than
	 *                              reporting a success that did not happen -- a silent success there
	 *                              would make the walk believe it reached the floor and stop asking.
	 * None                      -> nothing to do, reported as such.
	 *
	 * ⚠ THE CALLER STILL OWNS Commit(). FFPMGiveTakeWalk::Decide deliberately does not move the ladder
	 * position; the caller applies and then commits, so a refused or partial apply does not leave the
	 * walk believing a tier is engaged. Commit only on ChangedAnything().
	 */
	FFPMApplyResult Execute(const FFPMSteerDecision& Decision);

	/**
	 * Drop every hold the ladder owns, across every tier. The uninstall / OFF / world-change path.
	 * @return how many holds were dropped.
	 */
	int32 ReleaseEverything(const TCHAR* Reason);

	/** True while this tier has at least one live hold owned by the apply pass. */
	bool IsTierHeld(EFPMStageTier Tier) const;

	/**
	 * ★ ONE OWNER NAME PER TIER IDENTITY, derived rather than typed. Section 3.3: costs and cooldowns
	 * key by TIER IDENTITY, never by ladder position, and so does the ownership of a hold -- mode C
	 * moves K4f three places and its holds must follow the tier, not the slot.
	 */
	static FName OwnerFor(EFPMStageTier Tier);

	/**
	 * ★ THE LIVENESS PROOF, run at world load after the tables and the walk have proven themselves.
	 * Every check has a known-positive AND a known-negative, because this project has shipped four
	 * gates that could only ever refuse and nothing noticed.
	 *
	 *   1. A WRITE REALLY WRITES, AND A RELEASE REALLY RELEASES. On FPM's own probe cvar, never a game
	 *      cvar: hold through the apply pass's own ApplyOneLever, read the cvar back, release, read it
	 *      back. Value and holder must both return. The known-negative is the same check run against a
	 *      lever naming a cvar that does not exist, which must report WriterRefused and must NOT be
	 *      counted as written.
	 *   2. THE ANTI-RATCHET, MEASURED RATHER THAN ASSERTED. A BaseScale lever applied TWICE without a
	 *      release must land on the same value both times. The check also computes what a live-read
	 *      implementation would have produced and REQUIRES it to differ -- if the two coincide the
	 *      test proves nothing, and it fails rather than passing quietly.
	 *   3. AVAILABILITY IS OBEYED. A lever whose registry entry is not Available must be skipped, and
	 *      the skip must be counted as a skip rather than dropped.
	 *   4. THE GROUP FILTER IS ALIVE. FPMClassifyGroupMember must exclude the GI kill switch by name
	 *      and must NOT exclude an ordinary member -- the mirror half, without which "excludes
	 *      everything" would pass.
	 *   5. RESOLUTION IS REFUSED, NAMED. A ResolutionDown decision must come back not executed with a
	 *      reason, never as a success.
	 *   6. THE LEDGER IS CLEAN WHEN THE PASS ENDS. FPMCVarWriter's held set is captured before the
	 *      self-test and compared after; anything left behind is a leak and fails the test.
	 *
	 * @return true only if every check passed. ReportNow refuses to print a table if it did not.
	 */
	bool SelfTest();

	/** `FPM.Stage.Apply.Report` -- which tiers are held, session counters, and what the apply pass is
	 *  still waiting on before anything drives it. */
	void ReportNow(FOutputDevice& Ar) const;

	/** `FPM.Stage.Apply <tier> <engage|release>` / `FPM.Stage.Apply release-all`. The operator route,
	 *  and the way a boot proves the apply pass applies without a bench or a driver existing. */
	void ApplyFromConsole(const TArray<FString>& Args, FOutputDevice& Ar);

	/** Print one result, line by line, with its coverage counts. */
	static void PrintResult(const FFPMApplyResult& Result, FOutputDevice& Ar);

	/** Parse a tier name ("K1", "B5", "K4g"), case-insensitive. None when unrecognised. */
	static EFPMStageTier ParseTier(const FString& Text);

private:
	FFPMApplyResult EngageTier(EFPMStageTier Tier, EFPMSteerAction Action);
	FFPMApplyResult ReleaseTier(EFPMStageTier Tier, EFPMSteerAction Action);

	/** One cvar-backed lever. Returns true when a hold landed. */
	bool ApplyCVarLever(const FFPMStageLever& Lever, FName Owner, const TCHAR* Reason,
	                    FFPMApplyLine& OutLine);

	/** One group-backed lever: resolve the target tier, expand it, write the members. Adds its own
	 *  lines and coverage counts to `InOut`. */
	void ApplyGroupLever(const FFPMStageLever& Lever, FName Owner, const TCHAR* Reason,
	                     FFPMApplyResult& InOut);

	/** The live scalability tier of a group, read from `sg.<Group>`. INDEX_NONE when unreadable.
	 *  Reading is safe under Law 3 precisely because FPM can never write it (clause 2). */
	static int32 ReadLiveGroupTier(const FString& GroupName);

	bool bTierHeld[static_cast<int32>(EFPMStageTier::Count)] = {};

	bool  bSelfTestPassed = false;
	int32 SessionWrites = 0;
	int32 SessionReleases = 0;
	int32 SessionRefusals = 0;
};
