// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMStageTables.h"

/**
 * ★ SLICE 2 -- THE GIVE/TAKE WALK. Design section 3.1 (:263-269), section 3.2's gate terms
 * (:296-302), section 3.3's deadlock dissolution (:353-375).
 *
 * A mode is ONE PRIORITY LIST. GIVE = walk the list under load. TAKE = the exact reverse, LIFO. This
 * class is the thing that walks it: it holds the ladder POSITION, evaluates the gate terms against
 * one window of signal, and returns ONE decision with a reason. FPMStageTables holds the content and
 * the orders; this file holds the WHEN.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★★ HOW THE 0.55.0 DEADLOCK IS STRUCTURALLY GONE, AND WHY IT IS NOT JUST RETUNED
 *
 * The measured defect, on Ant's own save: the quality ladder NEVER engaged, in either direction.
 *     CUT   required  CurrentPct <= MinScreenPct + 0.1   (already at the resolution FLOOR)
 *     BOOST required  CurrentPct >= MaxScreenPct - 0.1   (already at FULL resolution)
 * Her resolution ran 67, 58, 50, 58 and touched neither end, so it was too high to cut and too low to
 * boost, forever. Both terms could only be true at an ENDPOINT the controller never reached.
 *
 * Three things here make that state impossible rather than unlikely:
 *
 * 1. THE RESOLUTION STEP IS IN THE ORDER, BEFORE THE CUTS. "At floor" is no longer a precondition a
 *    caller has to happen to satisfy; it is the state the PRECEDING step of the same walk exists to
 *    produce. Over budget and not at floor now has exactly one eligible action -- drive resolution
 *    down -- instead of no eligible action at all. The old code had no such step, which is why the
 *    term could sit false forever with nothing trying to make it true.
 *
 * 2. THE PROMOTE SIDE'S "AT FULL RESOLUTION" TERM IS DELETED, per section 3.3, and replaced by
 *    saturation. And saturation counts a bonus tier that CANNOT be promoted (absent cvar, unmet VRAM
 *    gate, inert on this machine) as satisfied rather than as outstanding. Requiring "every bonus
 *    engaged" would strand the resolution restore behind, for example, B6 being permanently
 *    unavailable on a 8GB card -- which is the same defect wearing different clothes.
 *
 * 3. EVERY DECISION RETURNS EITHER AN ACTION OR A NAMED BLOCK. There is no path that returns nothing
 *    and says nothing, and a self-test enumerates a grid of reachable states to prove it. A silent
 *    no-op is what let the original defect live for months.
 *
 * ⚠ WHAT IS STILL OUTSIDE THIS CLASS, AND SO STILL AT RISK. This walk does not own the resolution
 * lever; section 8 does, and its closure rule (native dyn-res handed our floor, else ownership moves
 * to the upscaler's rung ladder) is a Slice-3 investigation. If that executor never reaches the
 * floor, this walk will emit ResolutionDown forever. That is why FFPMSteerStall exists: it is the
 * instrument that would have caught the original bug in one session instead of never.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ⚠ WHAT THIS FILE DOES NOT BUILD. The signals that fill FFPMSteeringInputs (section 3.6, its own
 * Slice-2 bullet), the bench that fills FFPMTierMeasurement (section 4), mode selection and the UI
 * (section 6), CPU relief (section 3.7 -- a separate lever on a separate signal, and this walk must
 * never touch it), and THE APPLY PASS.
 *
 * The apply pass is named here rather than left as an omission: turning a decision into cvar writes
 * needs policy arithmetic (MaxOf/MinOf/BaseScale/BaseDelta) against a BASELINE, and Law 2 wants that
 * baseline from a shipped vanilla-defaults table which does not exist yet for engine r.* cvars
 * (FPMLeverTypes.h says so on EFPMLeverBaselineSource::ShippedTable). The guarded CapturedOnce path
 * exists and every stage lever already declares it, so the apply pass is a next step with a clear
 * shape, not a redesign. Building it today would mean either inventing a defaults table or live
 * reading a baseline, and the second is the exact ratchet Law 3 forbids.
 */

/**
 * ★ WHAT THE FRAME WAS WAITING ON. Section 3.6's hard-drop binder produces this; this walk only
 * consumes it.
 *
 * ⚠ Unknown IS NOT Cpu, and the two are separate values on purpose. Folding "the binder has not
 * answered" into "not GPU-bound" would make a dead binder look exactly like a genuinely CPU-bound
 * machine, and the walk would refuse every cut while reporting a reason that sounds correct.
 */
enum class EFPMBoundAttribution : uint8
{
	Unknown,
	Gpu,
	Cpu,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMBoundAttribution Attribution);

/**
 * ONE TIER'S MEASURED COST, keyed by TIER IDENTITY. The bench fills this (section 4); nothing here
 * fabricates it, and `bMeasured == false` is a first-class state rather than a zero pretending to be
 * a measurement.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMTierMeasurement
{
	bool  bMeasured = false;

	/** Cut side: frame milliseconds this tier gives BACK when engaged. */
	float RecoveryMs = 0.0f;

	/** Bonus side: frame milliseconds this tier SPENDS when promoted. */
	float CostMs = 0.0f;
};

/**
 * ONE WINDOW OF SIGNAL, plus the ladder's environment. Everything the walk needs and nothing it can
 * reach for itself, so a self-test can put it in any reachable state.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMSteeringInputs
{
	EFPMGovernorMode Mode = EFPMGovernorMode::Balanced;

	/** Wall/app clock seconds. Section 5.3's clock law: never factory time, which is clamped and so
	 *  diverges from real time exactly when the governor is acting. */
	double NowSeconds = 0.0;

	/** Section 3.6: the EMA of frame milliseconds, and THE ONLY STEERED SIGNAL. */
	float MeanFrameMs = 0.0f;

	/** BudgetMs = 1000/TargetFPS. RaiseMs = 1000/min(TargetFPS+15, MaxFPS). The gap between them IS
	 *  the dead-band, and section 3.6 requires it to clear the bench's measured A/A noise floor. */
	float BudgetMs = 16.667f;
	float RaiseMs  = 13.333f;

	EFPMBoundAttribution Attribution = EFPMBoundAttribution::Unknown;

	/** Section 3.2's "at floor": the resolution lever sits at AppliedMinScreenPct. Under native
	 *  dyn-res section 8's closure rule evaluates it as engine-reported-pct <= AppliedMin + 0.5.
	 *  The caller answers; this walk never guesses. */
	bool bResolutionAtFloor = false;

	/** True when resolution is at its ceiling, so the take side stops asking for more. */
	bool bResolutionAtMax = false;

	/** A bench profile exists for this mode. Section 3.5a: without one, no stage tier may move at all,
	 *  because "never worse than vanilla -- unproven means CUT" excludes every unmeasured lever. */
	bool bProfileAvailable = false;

	/** The bench's measured A/A noise floor in milliseconds. A recovery under this is indistinguishable
	 *  from noise, so spending a tier on it buys nothing. */
	float BenchNoiseFloorMs = 0.0f;

	/**
	 * Section 3.3's second disjunct, supplied by the caller: headroom exists that the next promotion
	 * structurally cannot absorb (a burn cooldown, for instance), held for the dwell. The walk computes
	 * its OWN first disjunct from the ladder position and treats these as alternatives, so a caller
	 * that never sets this cannot strand the resolution restore.
	 */
	bool bQualitySaturatedExternal = false;

	/** Per-tier bench measurements, indexed by tier identity. */
	FFPMTierMeasurement Measurements[static_cast<int32>(EFPMStageTier::Count)];

	FFPMTierMeasurement MeasurementFor(EFPMStageTier Tier) const;
};

/** What the walk decided to do with this window. */
enum class EFPMSteerAction : uint8
{
	None,
	DemoteBonus,      // GIVE: hand a bonus tier back
	EngageCut,        // GIVE: apply a cut tier
	ResolutionDown,   // GIVE: drive resolution toward AppliedMin. Section 8 executes it.
	ReleaseCut,       // TAKE: undo a cut tier
	PromoteBonus,     // TAKE: apply a bonus tier
	ResolutionUp,     // TAKE: resolution above the minimum (R+ in B/C). Section 8 executes it.
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMSteerAction Action);

/**
 * WHY NOTHING HAPPENED. A closed set, because "no action and no reason" is the failure mode this
 * whole file exists to make impossible. Every value below names a state a reader can act on.
 */
enum class EFPMSteerBlock : uint8
{
	None,

	/** The only LEGITIMATE idle state: the mean sits inside the dead-band between RaiseMs and
	 *  BudgetMs. Everything else here is something being waited on or refused. */
	InsideDeadBand,

	/** The over-budget or headroom state has not been held long enough yet. */
	Dwell,

	/** Section 3.5a: this mode carries no stage tiers at all. True of Balanced, by design. */
	ModeCarriesNoStageLevers,

	/** The binder has not attributed this window. Distinct from CpuBound on purpose. */
	AttributionUnknown,

	/** The binder says CPU. A GPU-side cut would not help, and CPU relief is a separate lever on a
	 *  separate signal (section 3.7) that this walk must never reach for. */
	CpuBound,

	/** Resolution is above the floor, so no cut is eligible yet. On the shipped orders this is
	 *  UNREACHABLE, because the resolution step precedes every cut tier and would have returned
	 *  ResolutionDown first. Reaching it means an order table put a cut before the resolution step. */
	AwaitingResolutionFloor,

	/** No bench profile, so no tier has a measured cost and none may move (section 3.5a). */
	NoBenchProfile,

	/** Every remaining candidate tier failed the bench-worthwhile test. */
	NothingWorthwhile,

	/** Every remaining candidate tier is inert on this machine (0 available levers). */
	AllCandidatesInert,

	/** Every remaining candidate tier is inside its own cooldown. */
	AllCandidatesOnCooldown,

	/** The ladder has bottomed out (everything cut) or topped out (everything restored). A real,
	 *  terminal state: the machine cannot do more in this direction. */
	LadderExhausted,
};
FICSITSPERFORMANCEMANAGER_API const TCHAR* LexToString(EFPMSteerBlock Block);

struct FICSITSPERFORMANCEMANAGER_API FFPMSteerDecision
{
	EFPMSteerAction Action = EFPMSteerAction::None;
	EFPMStageTier   Tier   = EFPMStageTier::None;
	EFPMSteerBlock  Block  = EFPMSteerBlock::None;

	/** Always populated, for both an action and a block. Written for whoever reads a support dump. */
	FString Reason;

	/**
	 * Mode A's "(slew, concurrent)" (design :283). In A the resolution slew runs ALONGSIDE the bonus
	 * demotions rather than behind them, so a DemoteBonus decision in A also carries this flag when
	 * resolution is still above the floor. Ignoring it costs the mode its defining behaviour: A is the
	 * mode where resolution is meant to be moving early.
	 */
	bool bResolutionSlewConcurrent = false;

	bool IsAction() const { return Action != EFPMSteerAction::None; }
};

/**
 * ★ THE DEADLOCK DETECTOR, and the reason this file can be trusted before a boot proves it.
 *
 * It watches for the SHAPE of the original bug rather than for the bug itself: a sustained
 * over-budget state in which the walk makes no progress. Two ways that happens, both counted here:
 *   (a) decision after decision returns a BLOCK while the mean is over budget, or
 *   (b) decision after decision returns the SAME action, and the state that action exists to change
 *       never changes. In practice that is ResolutionDown repeating while bResolutionAtFloor stays
 *       false, which is EXACTLY the 0.55.0 failure as it would appear in this architecture.
 *
 * WHAT CONCRETE INPUT MAKES IT FIRE: a caller that reports MeanFrameMs above BudgetMs and
 * bResolutionAtFloor false for longer than StallWarnSeconds. That is one afternoon on Ant's rig
 * under the old controller.
 * WOULD IT FIRE ON CORRECT INPUT: no. A working executor reaches the floor within a few decisions,
 * or the mean comes back inside the dead-band, and either resets the episode.
 *
 * Blocks that are EXPECTED (Balanced carrying no stage levers, no bench profile yet, a genuinely
 * CPU-bound window, a ladder that has honestly bottomed out) log at Warning; the rest log at Error,
 * because they are the shape that hid for months.
 */
/**
 * ★ ONE ROW OF THE DRY LADDER WALK. See FFPMGiveTakeWalk::DryRunWalk.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMDryWalkStep
{
	int32  Step = 0;
	double AtSeconds = 0.0;
	float  MeanFrameMs = 0.0f;

	FFPMSteerDecision Decision;

	/** One line per lever the decided tier moves: the lever, the old value, the new value, the note
	 *  and any AWAITING RULING marker. Empty for a decision that moved no tier. */
	TArray<FString> LeverLines;
};

/**
 * ★ THE RESULT OF A DRY LADDER WALK, INCLUDING ITS OWN PROOF THAT IT WROTE NOTHING.
 *
 * The held-cvar snapshot is taken from FPMCVarWriter before the first decision and again after the
 * last one. Identical means the walk held nothing new and released nothing. That is a claim about the
 * WRITER'S OWN LEDGER rather than about the walk's source, which is why the self-test also proves the
 * comparator can say NO: a comparator that always returns "identical" would report a clean walk after
 * a walk that wrote.
 */
struct FICSITSPERFORMANCEMANAGER_API FFPMDryWalkResult
{
	EFPMGovernorMode Mode = EFPMGovernorMode::Balanced;
	TArray<FFPMDryWalkStep> Steps;

	/** True when the walk stopped because the ladder settled, false when it hit the step budget. */
	bool bConverged = false;
	FString StopReason;

	/** FPMCVarWriter's held set, before and after. */
	TArray<FString> HeldBefore;
	TArray<FString> HeldAfter;
	bool bHeldSetIdentical = true;
	FString HeldSetDelta;

	/** The simulated cvar state must return to its starting values after a full give-then-take cycle.
	 *  This is the walk's own version of Law 3: a release restores the value that was SAVED before the
	 *  engage, never a value read back afterwards, so a repeated cycle cannot ratchet. */
	bool bReturnedToStart = true;
	FString RatchetDelta;

	/** The simulated GlobalIlluminationQuality tier never went below the floor during the walk. */
	bool bGIFloorHeld = true;
	int32 LowestGITierSeen = 0;
};

struct FICSITSPERFORMANCEMANAGER_API FFPMSteerStall
{
	bool   bActive = false;
	double StartedAtSeconds = 0.0;
	int32  Decisions = 0;
	EFPMSteerAction RepeatedAction = EFPMSteerAction::None;
	EFPMSteerBlock  RepeatedBlock = EFPMSteerBlock::None;
	bool   bReported = false;

	/** Episodes seen this session, so the report has a denominator instead of a single latest state. */
	int32  EpisodesTotal = 0;
	int32  EpisodesUnexpected = 0;
};

/**
 * ★ THE GATE TERMS, as free functions so each can be exercised on its own with synthetic inputs.
 *
 * They are not private helpers by accident. A gate term buried inside a decision function can only
 * ever be tested through that function, which is how a term that is true only at an unreachable
 * endpoint survives review: nobody can ask it a direct question.
 */
namespace FPMSteerGates
{
	/** Section 3.6: bOverBudget = frameMs > BudgetMs. Reachable whenever the machine misses target. */
	FICSITSPERFORMANCEMANAGER_API bool MeanOverBudget(const FFPMSteeringInputs& In);

	/** Section 3.6: bClearHeadroom = frameMs < RaiseMs. Reachable whenever it beats target by the
	 *  dead-band. Note the two are NOT complements: between them is the dead-band, where doing
	 *  nothing is correct. */
	FICSITSPERFORMANCEMANAGER_API bool MeanHasHeadroom(const FFPMSteeringInputs& In);

	/** How far over budget, in ms. Zero or negative when not over. */
	FICSITSPERFORMANCEMANAGER_API float OvershootMs(const FFPMSteeringInputs& In);

	/** Section 3.2: the hard-drop binder says GPU for this window. Unknown is NOT Gpu. */
	FICSITSPERFORMANCEMANAGER_API bool GpuBound(const FFPMSteeringInputs& In);

	/**
	 * ★ BENCH-WORTHWHILE (design :302), WITH ONE DELIBERATE DELTA FROM THE WRITTEN DESIGN.
	 *
	 * The design says: recovery at least the bench noise floor AND at least 25% of the overshoot.
	 * The second half of that, taken literally, REFUSES EVERY TIER on a large miss. At 30fps against
	 * a 60fps target the overshoot is 16.6ms, so 25% demands 4.15ms from a ladder whose largest rung
	 * is K3 at about 1.4ms. Nothing would ever be cut at exactly the moment a cut is most needed,
	 * and an unbalanced rig missing target by half is the case FPM exists for.
	 *
	 * The design already solved this shape on the PROMOTE side and did not mirror it here. Section 3.3
	 * (:353-366) gives bQualitySaturated a second disjunct -- "headroom exists that the next promotion
	 * structurally cannot absorb" -- and calls it "the load-bearing part: without it a burn-cooldown at
	 * +5 would strand R+ exactly the way bAtFullRes stranded the old ladder".
	 *
	 * So the cut side gets the same shape:
	 *     worthwhile = Recovery >= NoiseFloorMs
	 *              AND ( Recovery >= 0.25 * Overshoot
	 *                    OR 0.25 * Overshoot > BestRemainingRecoveryMs )
	 * The proportional term is dropped ONLY when no remaining tier could satisfy it, which is exactly
	 * the condition under which it stalls the ladder. The noise floor still binds in every case, so a
	 * recovery that cannot be told from noise is still refused.
	 *
	 * ⚠ THIS IS A DESIGN DELTA AND IT NEEDS ANT'S SIGN-OFF. It is written here rather than silently
	 * implemented as the design reads, because the design as written is a dead gate.
	 *
	 * @param BestRemainingRecoveryMs the largest measured recovery among tiers still available to
	 *        engage. 0 when nothing is left, which makes the second disjunct true and lets the walk
	 *        report LadderExhausted honestly rather than NothingWorthwhile misleadingly.
	 */
	FICSITSPERFORMANCEMANAGER_API bool BenchWorthwhile(const FFPMSteeringInputs& In,
	                                                   EFPMStageTier Candidate,
	                                                   float BestRemainingRecoveryMs,
	                                                   FString& OutWhy);

	/** The proportional fraction of the overshoot a cut must recover. [DEFAULT 0.25, design :302.] */
	FICSITSPERFORMANCEMANAGER_API float WorthwhileFraction();
}

class FICSITSPERFORMANCEMANAGER_API FFPMGiveTakeWalk final : public IFPMFix
{
public:
	static FFPMGiveTakeWalk& Get();

	virtual const TCHAR* Name() const override { return TEXT("give-take-walk"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }
	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Steering; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/**
	 * ONE WINDOW IN, ONE DECISION OUT. Call once per governor tick.
	 *
	 * Updates dwell and stall bookkeeping (which is why it is not const), but never changes the ladder
	 * position: the caller applies the decision and then calls Commit. That split exists so the apply
	 * pass can fail or refuse without the walk already believing it succeeded.
	 */
	FFPMSteerDecision Decide(const FFPMSteeringInputs& In);

	/** Record that `Decision` was actually carried out. Starts that TIER IDENTITY's cooldown
	 *  (section 3.3: costs and cooldowns key by tier identity, never by ladder position). */
	void Commit(const FFPMSteerDecision& Decision, double NowSeconds);

	bool IsEngaged(EFPMStageTier Tier) const;

	/** Tiers currently engaged, in tier order, for a report or a caller rebuilding its own view. */
	void GetEngaged(TArray<EFPMStageTier>& Out) const;

	const FFPMSteerStall& Stall() const { return StallState; }

	/**
	 * ★ THE NO-IDLE-BAND PROOF plus the gate-term liveness proofs. Runs at world load, after the
	 * stage tables' own self-test (so the orders it walks are already proven consistent).
	 *
	 * What it proves, and why each half is there:
	 *   1. NO SILENT NO-OP. A grid of reachable states across mode, signal, attribution, resolution
	 *      position, profile presence and ladder position. Every one must yield an action or a NAMED
	 *      block. A state that yielded neither is the exact failure the 0.55.0 ladder had.
	 *   2. THE 0.55.0 REGRESSION, both directions, on the historic numbers. Mid-band resolution (58%,
	 *      touching neither end), over budget, GPU-bound, profiled: mode A must return ResolutionDown.
	 *      Mid-band resolution with clear headroom: it must return an action, not nothing. The old
	 *      code returned nothing in both.
	 *   3. BENCH-WORTHWHILE, both directions AND the large-miss case. A tier under the noise floor is
	 *      refused (the floor still binds); a tier over it with a small overshoot passes; and the
	 *      16.6ms-overshoot case that the design as written refuses is proven to pass through the
	 *      second disjunct. Without the third the delta would be untested.
	 *   4. SATURATION DOES NOT STRAND THE RESTORE. With one bonus tier permanently inert, the take
	 *      side must still reach the resolution step. This is the mirror of the promote-side bug
	 *      section 3.3 names.
	 *   5. IDEMPOTENCE. Two decisions on a byte-identical input, with no Commit between them, must
	 *      agree on the action, the tier and the block. A walk that answers the same question two
	 *      ways cannot be reasoned about, and a caller retrying a failed apply would be handed a
	 *      different lever the second time.
	 *   6. THE DRY LADDER WALK, in all three steerable modes, with four things required of each run:
	 *      it CONVERGES, it moves a non-zero number of gives AND takes (a walk that moved nothing
	 *      would satisfy every other proof trivially), FPMCVarWriter's held set is byte-identical
	 *      before and after, and the simulated values return to where the first climb left them. The
	 *      held-set comparator is itself proven able to say NO against a perturbed copy.
	 *
	 * @return true only if every check passed. The report refuses to print if it did not.
	 */
	bool SelfTest();

	/** `FPM.Stage.Walk` -- ladder position, dwell, cooldowns, and the stall ledger. */
	void ReportNow(FOutputDevice& Ar) const;

	/** `FPM.Stage.Simulate` -- run ONE decision against injected inputs and print it, or with the
	 *  first argument `walk` run the full DRY LADDER WALK below. The walk is observable on the first
	 *  boot with no bench and no signals wired, which is the difference between a component that can
	 *  be checked and one that has to be believed. */
	void SimulateAndPrint(const TArray<FString>& Args, FOutputDevice& Ar);

	/**
	 * ★ THE DRY LADDER WALK. Drives one mode from vanilla to the bottom of its give order and back up
	 * its take order against a SYNTHETIC signal series, and records every step.
	 *
	 * WHAT IT IS FOR. 3071 lines of tier content and walk logic shipped with no driver: on a boot
	 * today the walk logs its arm line, runs its self-test and waits. This is how the whole ladder is
	 * exercised end to end without the signals, without the bench and without the apply pass, so a
	 * defect in the ORDERS or in the DECISION LOGIC is found offline instead of on Ant's rig.
	 *
	 * ⚠ IT IS DRY, AND THAT IS ENFORCED RATHER THAN INTENDED. It resolves a decision, projects the
	 * lever values through FPMProjectLeverValue against a SIMULATED cvar state that lives only inside
	 * this call, and prints them. It never calls FPMCVarWriter::Hold, Release, ReleaseOwner or
	 * ReleaseAll. FFPMDryWalkResult carries the writer's held set from before and after, and the
	 * self-test requires them identical.
	 *
	 * WHAT IS SYNTHETIC, STATED SO NOTHING HERE IS MISTAKEN FOR A MEASUREMENT:
	 *   - the signal series (a sustained over-budget phase, then a sustained headroom phase),
	 *   - the per-tier bench measurements (uniform, since no bench exists),
	 *   - the per-cvar starting values (a declared constant, not a live read, because a live read
	 *     would make the walk non-deterministic and could return FPM's own earlier write),
	 *   - the resolution executor, which is modelled as PERFECT: one ResolutionDown reaches the
	 *     floor. A stalled executor is the case FFPMSteerStall exists for, and it is not modelled
	 *     here because this walk would then never converge, which is the finding rather than a bug.
	 *
	 * The live ladder position, dwell state, cooldowns, stall ledger and session decision count are
	 * saved before and restored after, so a dry run leaves the session exactly as it found it.
	 *
	 * @param MaxSteps the step budget. Reaching it is reported as NOT converged, never as a pass.
	 */
	void DryRunWalk(EFPMGovernorMode Mode, int32 MaxSteps, FFPMDryWalkResult& Out);

	/** Print one dry walk, step by step, with every give and every take in order. */
	static void PrintDryRun(const FFPMDryWalkResult& Result, FOutputDevice& Ar);

	// ---- [DEFAULT] timings. Section 4.6 wants every default to name its mover; none of these has one
	// ---- yet, and the bench is the intended mover for the dwells. They are named here so that is
	// ---- visible rather than buried as literals in the decision function.
	static float GiveDwellSeconds()  { return 2.0f; }
	static float TakeDwellSeconds()  { return 5.0f; }
	static float TierCooldownSeconds() { return 10.0f; }
	static float StallWarnSeconds()  { return 30.0f; }

private:
	FFPMSteerDecision DecideGive(const FFPMSteeringInputs& In);
	FFPMSteerDecision DecideTake(const FFPMSteeringInputs& In);

	/** Section 3.3's first disjunct, computed from the ladder rather than taken on trust: every bonus
	 *  tier this mode carries is engaged OR cannot be engaged on this machine. The second half is what
	 *  stops a permanently inert tier from stranding the resolution restore. */
	bool IsQualitySaturated(const FFPMSteeringInputs& In) const;

	/** Largest measured recovery among cut tiers not yet engaged and not inert. */
	float BestRemainingRecoveryMs(const FFPMSteeringInputs& In) const;

	bool OnCooldown(EFPMStageTier Tier, double NowSeconds) const;
	void LogDecision(const FFPMSteeringInputs& In, const FFPMSteerDecision& Decision) const;
	void UpdateStall(const FFPMSteeringInputs& In, const FFPMSteerDecision& Decision);

	bool   bEngaged[static_cast<int32>(EFPMStageTier::Count)] = {};
	double CooldownUntil[static_cast<int32>(EFPMStageTier::Count)] = {};

	double OverBudgetSince = 0.0;
	double HeadroomSince   = 0.0;

	FFPMSteerStall StallState;
	bool bSelfTestPassed = false;
	int32 DecisionsThisSession = 0;
};
