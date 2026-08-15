// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

#include "Core/FPMFixContract.h"
#include "Core/FPMGiveTake.h"

/**
 * ★ SLICE 2 -- THE STEERING SIGNAL. Design section 3.6 (FPM2-DESIGN-ASSEMBLED.md:471-512).
 *
 * FFPMGiveTakeWalk consumes FFPMSteeringInputs and has never had anything to fill it. This file is
 * the producer: the smoothed frame mean, the budgets derived from the target frame rate, and the
 * 1% low telemetry. It decides nothing and it writes no console variable.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ THE CLOCK LAW, FIRST, BECAUSE IT IS THE ONE THAT SILENTLY POISONS EVERYTHING ELSE
 *
 * Section 3.6 / 5.3: every signal is measured on the FRAME clock, and factory time is NEVER used --
 * it is clamped, so it diverges from real time in proportion to hitching, which is largest exactly
 * when the governor is acting. This file reads FPlatformTime::Seconds() and nothing else. It does
 * not use the ticker's own delta argument either: that is the engine's SMOOTHED delta, and smoothing
 * a signal we then smooth ourselves makes the time constant a fiction.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY THE THREE PIECES OF ARITHMETIC ARE FREE FUNCTIONS
 *
 * FPMDeriveBudgets, FPMUpdateEma and FPMLowPercentileMs are pure: same inputs, same answer, no
 * globals touched. That is what lets the self-test ask each of them a DIRECT question with synthetic
 * numbers, instead of driving the whole subsystem and hoping. The give/take walk made the same
 * choice for its gate terms and stated the reason: a term buried inside its consumer can only be
 * tested through that consumer, which is how a term that is true only at an unreachable endpoint
 * survives review.
 *
 * It also means the self-test never has to WRITE a console variable to prove the budgets re-derive.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ★ BOARD m6138429, CLOSED BY SHAPE RATHER THAN BY A CALLBACK
 *
 * The known defect: `FPM.Set TargetFPS` never re-derived MaxFPS, so the budgets went stale against
 * the target the user had just chosen. The design's answer is "every TargetFPS change re-derives
 * both". This goes one step further and keeps NO derived state to go stale: the budgets are computed
 * from the live values every tick, in one function, and the cached copy exists only so a report can
 * print what the last tick used. There is no code path that can update one and not the other,
 * because there is no path that updates either.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────
 * ⚠ WHAT THIS FILE CANNOT SUPPLY, AND SAYS SO ON EVERY BuildInputs CALL
 *
 *   - ATTRIBUTION stays Unknown. The hard-drop binder (section 3.6's middle tier) needs the game-
 *     thread / render-thread busy split, which lives private inside FFPMHitchMeter. Unknown is a
 *     separate value from Cpu for exactly this reason: the walk refuses a cut on Unknown and names
 *     that as the block, rather than a dead binder looking like a CPU-bound machine.
 *   - bResolutionAtFloor stays false. Section 8 owns the resolution lever and its executor does not
 *     exist, so nothing here can honestly claim the floor was reached.
 *   - bProfileAvailable stays false. There is no bench (section 4), so no tier has a measured cost
 *     and section 3.5a excludes every one of them.
 *
 * Those three are why NOTHING DRIVES THE WALK YET even with this file in the tree, and why shipping
 * an automatic governor tick today would be a driver that can only ever report a named block. The
 * report says it in those words instead of leaving a reader to infer it from silence.
 */

/** The two thresholds and everything that went into them. Carrying the derivation with the numbers
 *  is what makes a wrong budget diagnosable from a support dump instead of a mystery. */
struct FICSITSPERFORMANCEMANAGER_API FFPMSteerBudgets
{
	/** 1000/TargetFPS, or the frame-cap form. Over this is bOverBudget. */
	float BudgetMs = 16.667f;

	/** 1000/(TargetFPS+spread), or the frame-cap form. Under this is bClearHeadroom. */
	float RaiseMs = 13.333f;

	float TargetFPS = 60.0f;

	/** t.MaxFPS as read, never written. Section 3.9 lists t.MaxFPS among the things the governor
	 *  never touches: it is US_*-backed, so FPM recommends a cap in the UI and never sets one.
	 *  0 or less means uncapped. */
	float MaxFPS = 0.0f;

	/** True when the frame-cap forms were used. */
	bool bFromFrameCap = false;

	/** Plain words, for the report. */
	FString Derivation;

	/** BudgetMs - RaiseMs. Section 3.6: this gap IS the dead-band, and the bench is required to widen
	 *  it if it does not clear the measured A/A noise floor. A dead-band of zero or less would make
	 *  "cut" and "boost" adjacent, which is an oscillator. */
	float DeadBandMs() const { return BudgetMs - RaiseMs; }
};

/**
 * ★ THE BUDGETS, AS ONE PURE FUNCTION. Design :477-479, carried from the FPM1 archive at :4057-4058
 * and :4104-4107.
 *
 *   uncapped:  BudgetMs = 1000/TargetFPS          RaiseMs = 1000/(TargetFPS + spread)
 *   capped:    BudgetMs = CapMs * 1.06            RaiseMs = CapMs * 1.02
 *
 * The +15 spread is the Normal utilisation mode (section 3.11). Boosted halves it to +7 and shortens
 * the promote dwells; neither is implemented, because the mode SELECTOR is section 6's surface and an
 * unreachable branch is not a feature. When it lands, this is the one function it changes.
 *
 * @param TargetFPS the user's target. Clamped to a sane range rather than trusted: a zero here would
 *        divide, and a settings surface that has not been built yet cannot be relied on to validate.
 * @param MaxFPS    t.MaxFPS as read. 0 or less means uncapped.
 */
FICSITSPERFORMANCEMANAGER_API FFPMSteerBudgets FPMDeriveBudgets(float TargetFPS, float MaxFPS);

/** The Normal-mode spread between the cut threshold and the boost threshold, in FPS.
 *  [DEFAULT 15, design :477. Mover: the bench's A/A noise floor at the release gate, section 4.6.] */
FICSITSPERFORMANCEMANAGER_API float FPMBudgetSpreadFPS();

/**
 * ★ ONE EMA STEP, FRAME-RATE INDEPENDENT ON PURPOSE.
 *
 * Alpha is derived from the elapsed time and a time constant -- alpha = 1 - exp(-dt/tau) -- rather
 * than being a fixed per-frame weight. A fixed weight makes the smoothing window depend on the frame
 * rate, so the mean would react four times faster at 240fps than at 60, and the dwell timings would
 * mean different things on different machines. The one thing FPM exists for is machines that are not
 * the same as each other.
 *
 * @param Current    the running mean, or a negative number to prime it from this sample.
 * @param SampleMs   this frame, in milliseconds.
 * @param DeltaSeconds elapsed since the last update.
 * @param TauSeconds the time constant.
 */
FICSITSPERFORMANCEMANAGER_API float FPMUpdateEma(float Current, float SampleMs, float DeltaSeconds,
                                                 float TauSeconds);

/**
 * ★ THE LOW PERCENTILE. Section 3.6: lows are TELEMETRY ONLY and are never steered on, because
 * steering on them was a one-way ratchet. The ruled term is the 1% LOW; the 0.1% is recorded beside
 * it as FPM1-lineage extra.
 *
 * "1% low" means the frame time at the 99th percentile of frame TIMES -- the slow tail. Percent is
 * given as 1.0 for the 1% low and 0.1 for the 0.1% low.
 *
 * @return -1 when the sample set is too small to support the percentile asked for. A made-up number
 *         from four samples is worse than an admission, and the caller prints the admission.
 */
FICSITSPERFORMANCEMANAGER_API float FPMLowPercentileMs(const TArray<float>& SamplesMs, float Percent);

class FICSITSPERFORMANCEMANAGER_API FFPMSteerSignal final : public IFPMFix
{
public:
	static FFPMSteerSignal& Get();

	virtual const TCHAR* Name() const override { return TEXT("steer-signal"); }

	/** A dedicated server renders nothing, so its frame clock says nothing about the GPU signals this
	 *  produces. The server's own lever lane is FPMServerLevers. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }
	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Steering; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;
	virtual void Disarm() override;

	/** The smoothed frame mean in milliseconds, or -1 before the first sample. */
	float MeanFrameMs() const { return MeanMs; }

	/** What the last tick derived. Never a cached decision -- see the header's m6138429 note. */
	const FFPMSteerBudgets& Budgets() const { return LastBudgets; }

	int32 SamplesThisSession() const { return SamplesSeen; }

	/**
	 * Fill everything section 3.6 can honestly answer, and leave the rest at the value that makes the
	 * walk refuse rather than guess.
	 *
	 * @param OutCoverage always written: what was filled, what was left, and why. A caller that
	 *        prints a decision without this line is publishing a verdict without its footnotes.
	 * @return false when the mean is not primed yet (fewer than MinSamplesToSteer frames). The inputs
	 *         are still filled so a report can show them; false means "do not steer on this".
	 */
	bool BuildInputs(EFPMGovernorMode Mode, FFPMSteeringInputs& Out, FString& OutCoverage) const;

	/**
	 * ★ THE LIVENESS PROOF. Each check asks the one question that separates a live instrument from a
	 * confident constant: what input would make this report a different number?
	 *
	 *   1. THE EMA MOVES, AND CONVERGES. Fed a constant it converges to it (known-positive); fed a
	 *      step it moves toward the new value and does NOT sit where it was (the mirror). A mean that
	 *      cannot move is the whole failure class.
	 *   2. THE EMA IS FRAME-RATE INDEPENDENT. The same elapsed time in one big step and in ten small
	 *      ones must land in the same place, within tolerance. Without this the dwell constants mean
	 *      different things on different machines.
	 *   3. THE BUDGETS DEPEND ON THE TARGET. 60 and 30 must produce different numbers, and the
	 *      60 case must produce 16.67/13.33 -- the arithmetic checked against a value a reader can
	 *      verify by hand rather than against itself.
	 *   4. THE DEAD-BAND IS POSITIVE IN EVERY DERIVATION, capped and uncapped. RaiseMs >= BudgetMs
	 *      would put "cut" and "boost" on the same side of one number, which is an oscillator.
	 *   5. THE FRAME CAP CHANGES THE ANSWER. A capped derivation must differ from the uncapped one at
	 *      the same target, or the cap branch is unreachable decoration.
	 *   6. THE PERCENTILE IS A PERCENTILE. A known series has a known 1% low; a series too short
	 *      returns -1 rather than a number; and a shuffled copy of the same series gives the same
	 *      answer, because a percentile that depends on input order is a bug wearing a plausible face.
	 */
	bool SelfTest();

	/** `FPM.Steer.Report` -- the live mean, the budgets and their derivation, the lows, and the
	 *  coverage statement of what this signal cannot supply. */
	void ReportNow(FOutputDevice& Ar) const;

	// ---- [DEFAULT]s, named here rather than buried as literals, per section 4.6. ----

	/** The EMA time constant. [DEFAULT 0.5s. Mover: the bench's A/A noise floor -- a tau that does not
	 *  outlast the noise makes the mean chase it.] */
	static float MeanTauSeconds() { return 0.5f; }

	/** Frames required before BuildInputs will say "steer on this". [DEFAULT 30 -- half a second at
	 *  60fps, so the mean has seen more than one tau.] */
	static int32 MinSamplesToSteer() { return 30; }

	/** How many recent frame times the low percentiles are computed over. [DEFAULT 3600 -- one minute
	 *  at 60fps. Enough that a 1% low means 36 frames rather than one outlier.] */
	static int32 LowWindowSamples() { return 3600; }

private:
	bool Tick(float SmoothedEngineDeltaDoNotUse);

	/** The running mean. Negative means unprimed, which is a state rather than a zero pretending to
	 *  be a fast frame. */
	float MeanMs = -1.0f;

	double LastSampleSeconds = 0.0;
	int32  SamplesSeen = 0;

	/** Largest single frame seen this session, printed beside the mean. Section 3.6 assigns the
	 *  stall/GC EXCLUSION to the hard-drop detector, not to the mean, so nothing is excluded here --
	 *  this number is how a reader sees what the mean absorbed. */
	float WorstSampleMs = 0.0f;

	/** Ring of recent samples for the lows. */
	TArray<float> LowWindow;
	int32 LowWindowNext = 0;

	FFPMSteerBudgets LastBudgets;

	FTSTicker::FDelegateHandle TickHandle;
	bool bSelfTestPassed = false;
};
