// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMGiveTake.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMCVarWriter.h"   // GetHeldCVars ONLY -- the dry walk proves it wrote nothing, and
                                   // that proof is the single reason this header is included here.
#include "Core/FPMDiag.h"
#include "Core/FPMLeverRegistry.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

const TCHAR* LexToString(const EFPMBoundAttribution Attribution)
{
	switch (Attribution)
	{
	case EFPMBoundAttribution::Unknown: return TEXT("unattributed");
	case EFPMBoundAttribution::Gpu:     return TEXT("GPU-bound");
	case EFPMBoundAttribution::Cpu:     return TEXT("CPU-bound");
	default:                            return TEXT("<unknown attribution>");
	}
}

const TCHAR* LexToString(const EFPMSteerAction Action)
{
	switch (Action)
	{
	case EFPMSteerAction::None:           return TEXT("none");
	case EFPMSteerAction::DemoteBonus:    return TEXT("demote-bonus");
	case EFPMSteerAction::EngageCut:      return TEXT("engage-cut");
	case EFPMSteerAction::ResolutionDown: return TEXT("resolution-down");
	case EFPMSteerAction::ReleaseCut:     return TEXT("release-cut");
	case EFPMSteerAction::PromoteBonus:   return TEXT("promote-bonus");
	case EFPMSteerAction::ResolutionUp:   return TEXT("resolution-up");
	default:                              return TEXT("<unknown action>");
	}
}

const TCHAR* LexToString(const EFPMSteerBlock Block)
{
	switch (Block)
	{
	case EFPMSteerBlock::None:                     return TEXT("none");
	case EFPMSteerBlock::InsideDeadBand:           return TEXT("inside-dead-band");
	case EFPMSteerBlock::Dwell:                    return TEXT("dwell");
	case EFPMSteerBlock::ModeCarriesNoStageLevers: return TEXT("mode-carries-no-stage-levers");
	case EFPMSteerBlock::AttributionUnknown:       return TEXT("attribution-unknown");
	case EFPMSteerBlock::CpuBound:                 return TEXT("cpu-bound");
	case EFPMSteerBlock::AwaitingResolutionFloor:  return TEXT("awaiting-resolution-floor");
	case EFPMSteerBlock::NoBenchProfile:           return TEXT("no-bench-profile");
	case EFPMSteerBlock::NothingWorthwhile:        return TEXT("nothing-worthwhile");
	case EFPMSteerBlock::AllCandidatesInert:       return TEXT("all-candidates-inert");
	case EFPMSteerBlock::AllCandidatesOnCooldown:  return TEXT("all-candidates-on-cooldown");
	case EFPMSteerBlock::LadderExhausted:          return TEXT("ladder-exhausted");
	default:                                       return TEXT("<unknown block>");
	}
}

FFPMTierMeasurement FFPMSteeringInputs::MeasurementFor(const EFPMStageTier Tier) const
{
	const int32 I = static_cast<int32>(Tier);
	return (I >= 0 && I < static_cast<int32>(EFPMStageTier::Count))
		? Measurements[I] : FFPMTierMeasurement();
}

namespace
{
	// ⚠ NAMED WalkTierIdx AND NOT Idx. UE unity builds concatenate translation units, so two
	// anonymous-namespace helpers with the same signature in two .cpp files in the same unity chunk
	// are a redefinition error rather than two private helpers. FPMStageTables.cpp owns StageIdx.
	int32 WalkTierIdx(const EFPMStageTier Tier) { return static_cast<int32>(Tier); }
}

// ------------------------------------------------------------------------------------------------
// The gate terms. Free functions on purpose: a term that can only be reached through the decision
// function that uses it cannot be asked a direct question, and that is how a term true only at an
// unreachable endpoint survives a review.
// ------------------------------------------------------------------------------------------------

namespace FPMSteerGates
{
	bool MeanOverBudget(const FFPMSteeringInputs& In)
	{
		// REACHABLE WHEN: the machine misses its target frame time for the window. Nothing about this
		// requires any lever to be at any endpoint, which is the whole difference from the term it
		// replaces.
		return In.MeanFrameMs > In.BudgetMs;
	}

	bool MeanHasHeadroom(const FFPMSteeringInputs& In)
	{
		// REACHABLE WHEN: the machine beats target by more than the dead-band. NOT the complement of
		// MeanOverBudget: between RaiseMs and BudgetMs sits the dead-band, where doing nothing is the
		// right answer and the walk says so by name.
		return In.MeanFrameMs < In.RaiseMs;
	}

	float OvershootMs(const FFPMSteeringInputs& In)
	{
		return In.MeanFrameMs - In.BudgetMs;
	}

	bool GpuBound(const FFPMSteeringInputs& In)
	{
		// REACHABLE WHEN: section 3.6's hard-drop binder attributes the window to the GPU. Unknown is
		// deliberately not folded in here; see EFPMSteerBlock::AttributionUnknown.
		return In.Attribution == EFPMBoundAttribution::Gpu;
	}

	float WorthwhileFraction()
	{
		return 0.25f;
	}

	bool BenchWorthwhile(const FFPMSteeringInputs& In, const EFPMStageTier Candidate,
	                     const float BestRemainingRecoveryMs, FString& OutWhy)
	{
		const FFPMTierMeasurement M = In.MeasurementFor(Candidate);
		if (!M.bMeasured)
		{
			OutWhy = TEXT("no bench measurement for this tier, so its recovery is unknown rather than "
			              "zero. Unproven means the tier does not move.");
			return false;
		}

		// The noise floor binds ALWAYS, in both branches below. A recovery that cannot be told from
		// the bench's own A/A noise buys nothing measurable, whatever the miss looks like.
		if (M.RecoveryMs < In.BenchNoiseFloorMs)
		{
			OutWhy = FString::Printf(
				TEXT("measured recovery %.3fms is under the bench noise floor %.3fms, so it is not "
				     "distinguishable from noise"), M.RecoveryMs, In.BenchNoiseFloorMs);
			return false;
		}

		const float Overshoot = OvershootMs(In);
		const float Proportional = WorthwhileFraction() * Overshoot;

		if (M.RecoveryMs >= Proportional)
		{
			OutWhy = FString::Printf(
				TEXT("measured recovery %.3fms covers at least %.0f%% of the %.3fms overshoot"),
				M.RecoveryMs, WorthwhileFraction() * 100.0f, Overshoot);
			return true;
		}

		// ★ THE DESIGN DELTA (see the header for the full argument and the receipt). The proportional
		// term is dropped ONLY when no tier still available could satisfy it, which is exactly the
		// state in which it stalls the ladder instead of protecting it.
		if (Proportional > BestRemainingRecoveryMs)
		{
			OutWhy = FString::Printf(
				TEXT("the %.0f%% term wants %.3fms but the best remaining tier recovers %.3fms, so no "
				     "tier could satisfy it and applying it would stall the ladder at the exact moment "
				     "the miss is largest. Noise floor still cleared (%.3fms >= %.3fms). "
				     "[AWAITING RULING: bench-worthwhile-disjunct] [DESIGN DELTA from :302, mirroring "
				     "section 3.3's saturation disjunct]"),
				WorthwhileFraction() * 100.0f, Proportional, BestRemainingRecoveryMs,
				M.RecoveryMs, In.BenchNoiseFloorMs);
			return true;
		}

		OutWhy = FString::Printf(
			TEXT("measured recovery %.3fms is under the %.3fms this miss asks of a tier, and a bigger "
			     "tier (best remaining %.3fms) can still be reached"),
			M.RecoveryMs, Proportional, BestRemainingRecoveryMs);
		return false;
	}
}

FFPMGiveTakeWalk& FFPMGiveTakeWalk::Get()
{
	static FFPMGiveTakeWalk Instance;
	return Instance;
}

bool FFPMGiveTakeWalk::IsEngaged(const EFPMStageTier Tier) const
{
	const int32 I = WalkTierIdx(Tier);
	return (I >= 0 && I < WalkTierIdx(EFPMStageTier::Count)) && bEngaged[I];
}

void FFPMGiveTakeWalk::GetEngaged(TArray<EFPMStageTier>& Out) const
{
	for (int32 I = 0; I < WalkTierIdx(EFPMStageTier::Count); ++I)
	{
		if (bEngaged[I])
		{
			Out.Add(static_cast<EFPMStageTier>(I));
		}
	}
}

bool FFPMGiveTakeWalk::OnCooldown(const EFPMStageTier Tier, const double NowSeconds) const
{
	const int32 I = WalkTierIdx(Tier);
	return (I >= 0 && I < WalkTierIdx(EFPMStageTier::Count)) && NowSeconds < CooldownUntil[I];
}

float FFPMGiveTakeWalk::BestRemainingRecoveryMs(const FFPMSteeringInputs& In) const
{
	const FFPMStageTables& Tables = FFPMStageTables::Get();
	float Best = 0.0f;
	for (const EFPMStageTier Tier : Tables.GiveOrder(In.Mode))
	{
		if (!FPMIsCutTier(Tier) || IsEngaged(Tier))
		{
			continue;
		}
		FString InertWhy;
		if (Tables.IsTierInert(Tier, InertWhy))
		{
			continue;
		}
		const FFPMTierMeasurement M = In.MeasurementFor(Tier);
		if (M.bMeasured && M.RecoveryMs > Best)
		{
			Best = M.RecoveryMs;
		}
	}
	return Best;
}

/**
 * ★ SECTION 3.3's FIRST DISJUNCT, COMPUTED RATHER THAN TAKEN ON TRUST, and the place the old
 * deadlock would come back if it were written carelessly.
 *
 * The naive reading is "every bonus tier is engaged". That reading STRANDS the resolution restore the
 * moment one bonus tier can never be engaged on this machine, and B6 is exactly that tier on any card
 * under 11.5GB because its VRAM gate refuses it. The player would sit below full resolution forever
 * with the ladder reporting that it was still climbing.
 *
 * So a bonus tier counts as satisfied when it is engaged, OR inert here, OR unmeasured, OR measured
 * and too expensive for the headroom that exists. Those are the four ways "the next promotion
 * structurally cannot absorb the headroom" happens, which is section 3.3's own wording (:360).
 */
bool FFPMGiveTakeWalk::IsQualitySaturated(const FFPMSteeringInputs& In) const
{
	const FFPMStageTables& Tables = FFPMStageTables::Get();
	const float Headroom = In.RaiseMs - In.MeanFrameMs;

	for (const EFPMStageTier Tier : Tables.GiveOrder(In.Mode))
	{
		if (!FPMIsBonusTier(Tier) || IsEngaged(Tier))
		{
			continue;
		}
		FString InertWhy;
		if (Tables.IsTierInert(Tier, InertWhy))
		{
			continue;
		}
		const FFPMTierMeasurement M = In.MeasurementFor(Tier);
		if (!M.bMeasured)
		{
			continue;
		}
		if (M.CostMs > Headroom)
		{
			continue;
		}
		// This tier could be promoted right now, so quality is not saturated.
		return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// THE WALK.
// ------------------------------------------------------------------------------------------------

FFPMSteerDecision FFPMGiveTakeWalk::Decide(const FFPMSteeringInputs& In)
{
	++DecisionsThisSession;

	FFPMSteerDecision Decision;
	const bool bOver = FPMSteerGates::MeanOverBudget(In);
	const bool bHeadroom = FPMSteerGates::MeanHasHeadroom(In);

	if (!bOver)     { OverBudgetSince = -1.0; }
	if (!bHeadroom) { HeadroomSince   = -1.0; }

	if (!bOver && !bHeadroom)
	{
		Decision.Block = EFPMSteerBlock::InsideDeadBand;
		Decision.Reason = FString::Printf(
			TEXT("mean %.2fms sits inside the dead-band [%.2f raise, %.2f budget]. This is the ONE "
			     "state in which doing nothing is the answer rather than something being waited on."),
			In.MeanFrameMs, In.RaiseMs, In.BudgetMs);
		UpdateStall(In, Decision);
		LogDecision(In, Decision);
		return Decision;
	}

	if (bOver)
	{
		if (OverBudgetSince < 0.0) { OverBudgetSince = In.NowSeconds; }
		const double Held = In.NowSeconds - OverBudgetSince;
		if (Held < GiveDwellSeconds())
		{
			Decision.Block = EFPMSteerBlock::Dwell;
			Decision.Reason = FString::Printf(
				TEXT("over budget for %.2fs of the %.2fs give dwell. One noisy window is not a load "
				     "change."), Held, GiveDwellSeconds());
			UpdateStall(In, Decision);
			LogDecision(In, Decision);
			return Decision;
		}
		Decision = DecideGive(In);
		UpdateStall(In, Decision);
		LogDecision(In, Decision);
		return Decision;
	}

	if (HeadroomSince < 0.0) { HeadroomSince = In.NowSeconds; }
	const double Held = In.NowSeconds - HeadroomSince;
	if (Held < TakeDwellSeconds())
	{
		Decision.Block = EFPMSteerBlock::Dwell;
		Decision.Reason = FString::Printf(
			TEXT("clear headroom for %.2fs of the %.2fs take dwell. Taking back is deliberately slower "
			     "than giving up, so a brief lull does not buy something the next second cannot keep."),
			Held, TakeDwellSeconds());
		UpdateStall(In, Decision);
		LogDecision(In, Decision);
		return Decision;
	}

	Decision = DecideTake(In);
	UpdateStall(In, Decision);
	LogDecision(In, Decision);
	return Decision;
}

/**
 * Per-decision detail, at FPM.Diag.Steering 2. Level 1 carries the arm lines, both self-test results
 * and every STALL episode; level 2 is one line per governor tick, which is why it is not the default.
 */
void FFPMGiveTakeWalk::LogDecision(const FFPMSteeringInputs& In, const FFPMSteerDecision& Decision) const
{
	if (!FPMDiag::IsOn(FPMDiag::EChannel::Steering, 2))
	{
		return;
	}
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] give/take: mode %s mean %.2fms budget %.2fms %s floor=%s -> action '%s' tier '%s' "
		     "block '%s'. %s"),
		LexToString(In.Mode), In.MeanFrameMs, In.BudgetMs, LexToString(In.Attribution),
		In.bResolutionAtFloor ? TEXT("yes") : TEXT("no"),
		LexToString(Decision.Action), LexToString(Decision.Tier), LexToString(Decision.Block),
		*Decision.Reason);
}

FFPMSteerDecision FFPMGiveTakeWalk::DecideGive(const FFPMSteeringInputs& In)
{
	FFPMSteerDecision Decision;
	const FFPMStageTables& Tables = FFPMStageTables::Get();
	const TArray<EFPMStageTier>& Order = Tables.GiveOrder(In.Mode);

	bool bModeHasStageTiers = false;
	for (const EFPMStageTier Tier : Order)
	{
		if (FPMIsBonusTier(Tier) || FPMIsCutTier(Tier)) { bModeHasStageTiers = true; break; }
	}

	int32 InertCount = 0;
	int32 CooldownCount = 0;
	int32 NotWorthwhileCount = 0;
	int32 RemainingCuts = 0;
	FString FirstNotWorthwhile;

	const float BestRemaining = BestRemainingRecoveryMs(In);

	for (const EFPMStageTier Tier : Order)
	{
		// ---- THE RESOLUTION STEP. Its POSITION in the order is what dissolves the deadlock: over
		// ---- budget and above the floor now has exactly one eligible action instead of none.
		if (Tier == EFPMStageTier::Resolution)
		{
			if (!In.bResolutionAtFloor)
			{
				Decision.Action = EFPMSteerAction::ResolutionDown;
				Decision.Tier = Tier;
				Decision.Reason = FString::Printf(
					TEXT("over budget by %.2fms and resolution is above AppliedMin. Section 8 owns the "
					     "lever; this walk owns the order, and the resolution step sits before every "
					     "cut tier so 'at floor' is PRODUCED rather than waited for."),
					FPMSteerGates::OvershootMs(In));
				return Decision;
			}
			continue;
		}

		// ---- BONUS TIERS: under load they are handed back, highest first.
		if (FPMIsBonusTier(Tier))
		{
			if (!IsEngaged(Tier)) { continue; }
			if (OnCooldown(Tier, In.NowSeconds)) { ++CooldownCount; continue; }

			Decision.Action = EFPMSteerAction::DemoteBonus;
			Decision.Tier = Tier;
			Decision.bResolutionSlewConcurrent =
				(In.Mode == EFPMGovernorMode::ResolutionFirst) && !In.bResolutionAtFloor;
			Decision.Reason = FString::Printf(
				TEXT("over budget by %.2fms; %s is engaged and is the highest bonus still standing.%s"),
				FPMSteerGates::OvershootMs(In), LexToString(Tier),
				Decision.bResolutionSlewConcurrent
					? TEXT(" Mode A slews resolution CONCURRENTLY with this demotion (design :283).")
					: TEXT(""));
			return Decision;
		}

		// ---- CUT TIERS.
		if (!FPMIsCutTier(Tier)) { continue; }
		if (IsEngaged(Tier)) { continue; }
		++RemainingCuts;

		// Gates that apply to EVERY cut, evaluated at the first cut candidate rather than up front, so
		// a mode whose bonuses still had work never reports a cut-side block it never reached.
		if (In.Attribution == EFPMBoundAttribution::Unknown)
		{
			Decision.Block = EFPMSteerBlock::AttributionUnknown;
			Decision.Reason = TEXT("over budget, at floor, but the hard-drop binder has not attributed "
			                       "this window. Unknown is NOT treated as GPU: a dead binder would "
			                       "otherwise cut quality on a CPU-bound machine forever.");
			return Decision;
		}
		if (In.Attribution == EFPMBoundAttribution::Cpu)
		{
			Decision.Block = EFPMSteerBlock::CpuBound;
			Decision.Reason = TEXT("over budget and at floor, but the window is CPU-bound. A GPU-side "
			                       "cut would not move the frame time, and CPU relief is a separate "
			                       "lever on a separate signal (section 3.7) that this walk never "
			                       "reaches for.");
			return Decision;
		}
		if (!In.bResolutionAtFloor)
		{
			// UNREACHABLE ON THE SHIPPED ORDERS, and that is checkable rather than hoped: the
			// resolution step precedes every cut tier in A, B and C, so this loop returns
			// ResolutionDown before it can arrive here. Getting here means an ORDER TABLE put a cut
			// ahead of the resolution step, which is a table defect and is named as one.
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take: reached cut tier %s in %s with resolution ABOVE the floor. The "
				     "resolution step must precede every cut tier in every mode order; this one does "
				     "not, and the cut gate is the only thing standing between that table defect and "
				     "the 0.55.0 deadlock."), LexToString(Tier), LexToString(In.Mode));
			Decision.Block = EFPMSteerBlock::AwaitingResolutionFloor;
			Decision.Reason = TEXT("a cut tier was reached while resolution is above the floor, which "
			                       "the mode order should have made impossible");
			return Decision;
		}
		if (!In.bProfileAvailable)
		{
			Decision.Block = EFPMSteerBlock::NoBenchProfile;
			Decision.Reason = TEXT("no bench profile, so no tier has a measured recovery. Section 3.5a: "
			                       "an unproven lever does not move, whatever the frame time says.");
			return Decision;
		}

		FString InertWhy;
		if (Tables.IsTierInert(Tier, InertWhy))
		{
			++InertCount;
			continue;
		}
		if (OnCooldown(Tier, In.NowSeconds))
		{
			++CooldownCount;
			continue;
		}

		FString Why;
		if (!FPMSteerGates::BenchWorthwhile(In, Tier, BestRemaining, Why))
		{
			++NotWorthwhileCount;
			if (FirstNotWorthwhile.IsEmpty())
			{
				FirstNotWorthwhile = FString::Printf(TEXT("%s: %s"), LexToString(Tier), *Why);
			}
			continue;
		}

		Decision.Action = EFPMSteerAction::EngageCut;
		Decision.Tier = Tier;
		Decision.Reason = FString::Printf(TEXT("over budget by %.2fms, GPU-bound, at floor. %s"),
			FPMSteerGates::OvershootMs(In), *Why);
		return Decision;
	}

	// Nothing was eligible. Name WHY, most informative first. Every branch below is a state a reader
	// can act on, and there is no fall-through that returns silence.
	if (!bModeHasStageTiers)
	{
		Decision.Block = EFPMSteerBlock::ModeCarriesNoStageLevers;
		Decision.Reason = FString::Printf(
			TEXT("%s carries no stage tiers at all. Section 3.5a: before a bench exists no tier has a "
			     "measured cost, so resolution is the only GPU-side lever this mode steers, and it is "
			     "already at the floor."), LexToString(In.Mode));
	}
	else if (RemainingCuts == 0)
	{
		Decision.Block = EFPMSteerBlock::LadderExhausted;
		Decision.Reason = TEXT("every bonus is demoted and every cut tier is engaged. The ladder has "
		                       "honestly bottomed out; this machine cannot give more without a lever "
		                       "the design does not have.");
	}
	else if (NotWorthwhileCount > 0)
	{
		Decision.Block = EFPMSteerBlock::NothingWorthwhile;
		Decision.Reason = FString::Printf(
			TEXT("%d candidate tier(s) failed bench-worthwhile. First: %s"),
			NotWorthwhileCount, *FirstNotWorthwhile);
	}
	else if (CooldownCount > 0)
	{
		Decision.Block = EFPMSteerBlock::AllCandidatesOnCooldown;
		Decision.Reason = FString::Printf(
			TEXT("%d candidate tier(s), all inside their own cooldown. Cooldowns key by TIER IDENTITY, "
			     "so a mode that reorders tiers cannot accidentally reset one."), CooldownCount);
	}
	else if (InertCount > 0)
	{
		Decision.Block = EFPMSteerBlock::AllCandidatesInert;
		Decision.Reason = FString::Printf(
			TEXT("%d candidate tier(s), all inert on this machine. Run FPM.Stage.Report for the "
			     "per-tier reason."), InertCount);
	}
	else
	{
		Decision.Block = EFPMSteerBlock::LadderExhausted;
		Decision.Reason = TEXT("no eligible step remains in this mode's give order");
	}
	return Decision;
}

FFPMSteerDecision FFPMGiveTakeWalk::DecideTake(const FFPMSteeringInputs& In)
{
	FFPMSteerDecision Decision;
	const FFPMStageTables& Tables = FFPMStageTables::Get();
	const TArray<EFPMStageTier>& Order = Tables.TakeOrder(In.Mode);
	const float Headroom = In.RaiseMs - In.MeanFrameMs;

	bool bModeHasStageTiers = false;
	for (const EFPMStageTier Tier : Order)
	{
		if (FPMIsBonusTier(Tier) || FPMIsCutTier(Tier)) { bModeHasStageTiers = true; break; }
	}

	int32 InertCount = 0;
	int32 CooldownCount = 0;
	int32 UnaffordableCount = 0;
	int32 UnmeasuredCount = 0;
	int32 RemainingSteps = 0;
	bool bResolutionHeldBySaturation = false;

	for (const EFPMStageTier Tier : Order)
	{
		if (Tier == EFPMStageTier::Resolution)
		{
			if (In.bResolutionAtMax) { continue; }
			++RemainingSteps;

			// R+ exists only in B and C, where resolution was PINNED at the floor on the way down and
			// above-minimum is the ladder's virtual top rung (section 3.3). In A and Balanced the
			// resolution restore sits mid-order and needs no saturation term at all, because the
			// bonuses it precedes are restored after it by construction.
			const bool bNeedsSaturation = (In.Mode == EFPMGovernorMode::GraphicsFirst)
				|| (In.Mode == EFPMGovernorMode::LightingFirst);
			if (bNeedsSaturation && !IsQualitySaturated(In) && !In.bQualitySaturatedExternal)
			{
				bResolutionHeldBySaturation = true;
				continue;
			}

			Decision.Action = EFPMSteerAction::ResolutionUp;
			Decision.Tier = Tier;
			Decision.Reason = FString::Printf(
				TEXT("clear headroom of %.2fms and quality is saturated%s. Section 3.3's 'at full "
				     "resolution' term is DELETED; saturation replaces it, and a bonus tier that "
				     "cannot be promoted on this machine counts as satisfied rather than outstanding."),
				Headroom, In.bQualitySaturatedExternal ? TEXT(" (caller-supplied disjunct)") : TEXT(""));
			return Decision;
		}

		if (FPMIsCutTier(Tier))
		{
			if (!IsEngaged(Tier)) { continue; }
			++RemainingSteps;
			if (OnCooldown(Tier, In.NowSeconds)) { ++CooldownCount; continue; }

			Decision.Action = EFPMSteerAction::ReleaseCut;
			Decision.Tier = Tier;
			Decision.Reason = FString::Printf(
				TEXT("clear headroom of %.2fms; %s is the most recent cut still standing. The take "
				     "order is the exact reverse of the give order, so LIFO is structural rather than "
				     "a stack somebody has to keep correct."), Headroom, LexToString(Tier));
			return Decision;
		}

		if (!FPMIsBonusTier(Tier)) { continue; }
		if (IsEngaged(Tier)) { continue; }
		++RemainingSteps;

		if (!In.bProfileAvailable)
		{
			Decision.Block = EFPMSteerBlock::NoBenchProfile;
			Decision.Reason = TEXT("no bench profile, so no bonus tier has a measured cost and none may "
			                       "be promoted. Section 3.5a.");
			return Decision;
		}

		FString InertWhy;
		if (Tables.IsTierInert(Tier, InertWhy)) { ++InertCount; continue; }
		if (OnCooldown(Tier, In.NowSeconds)) { ++CooldownCount; continue; }

		const FFPMTierMeasurement M = In.MeasurementFor(Tier);
		if (!M.bMeasured) { ++UnmeasuredCount; continue; }
		if (M.CostMs > Headroom)
		{
			// The promote currency (board m508019310): a lever is affordable when its MEASURED cost
			// fits the headroom that actually exists, never when it merely looks small.
			++UnaffordableCount;
			continue;
		}

		Decision.Action = EFPMSteerAction::PromoteBonus;
		Decision.Tier = Tier;
		Decision.Reason = FString::Printf(
			TEXT("clear headroom of %.2fms and %s costs a measured %.3fms, which fits."),
			Headroom, LexToString(Tier), M.CostMs);
		return Decision;
	}

	if (!bModeHasStageTiers)
	{
		Decision.Block = EFPMSteerBlock::ModeCarriesNoStageLevers;
		Decision.Reason = FString::Printf(
			TEXT("%s carries no stage tiers, and resolution is already at its maximum."),
			LexToString(In.Mode));
	}
	else if (RemainingSteps == 0)
	{
		Decision.Block = EFPMSteerBlock::LadderExhausted;
		Decision.Reason = TEXT("every cut is released, every bonus is promoted and resolution is at its "
		                       "maximum. The ladder has honestly topped out.");
	}
	else if (bResolutionHeldBySaturation)
	{
		Decision.Block = EFPMSteerBlock::LadderExhausted;
		Decision.Reason = TEXT("headroom exists, but quality is not saturated and every promotable "
		                       "bonus is inert, unmeasured, on cooldown or unaffordable. Resolution is "
		                       "deliberately held back until quality is saturated (section 3.3).");
	}
	else if (UnaffordableCount > 0)
	{
		Decision.Block = EFPMSteerBlock::NothingWorthwhile;
		Decision.Reason = FString::Printf(
			TEXT("%d bonus tier(s) available, all costing more than the %.2fms of headroom."),
			UnaffordableCount, Headroom);
	}
	else if (UnmeasuredCount > 0)
	{
		Decision.Block = EFPMSteerBlock::NoBenchProfile;
		Decision.Reason = FString::Printf(
			TEXT("%d bonus tier(s) have no measured cost in this profile."), UnmeasuredCount);
	}
	else if (CooldownCount > 0)
	{
		Decision.Block = EFPMSteerBlock::AllCandidatesOnCooldown;
		Decision.Reason = FString::Printf(TEXT("%d candidate tier(s), all inside their own cooldown."),
			CooldownCount);
	}
	else if (InertCount > 0)
	{
		Decision.Block = EFPMSteerBlock::AllCandidatesInert;
		Decision.Reason = FString::Printf(
			TEXT("%d candidate tier(s), all inert on this machine."), InertCount);
	}
	else
	{
		Decision.Block = EFPMSteerBlock::LadderExhausted;
		Decision.Reason = TEXT("no eligible step remains in this mode's take order");
	}
	return Decision;
}

void FFPMGiveTakeWalk::Commit(const FFPMSteerDecision& Decision, const double NowSeconds)
{
	const int32 I = WalkTierIdx(Decision.Tier);
	if (I <= 0 || I >= WalkTierIdx(EFPMStageTier::Count))
	{
		return;
	}

	switch (Decision.Action)
	{
	case EFPMSteerAction::EngageCut:
	case EFPMSteerAction::PromoteBonus:
		bEngaged[I] = true;
		break;
	case EFPMSteerAction::ReleaseCut:
	case EFPMSteerAction::DemoteBonus:
		bEngaged[I] = false;
		break;
	default:
		// ⚠ RESOLUTION TAKES NO COOLDOWN, and that is a decision rather than an omission. Section 8
		// owns the slew rate; a cooldown here would be a SECOND, uncoordinated rate limiter over the
		// same lever. Worse, the give walk would then have to skip the resolution step while it
		// cooled, fall through to the cut tiers with resolution still above the floor, and trip the
		// very table-defect guard that exists to catch a cut ordered ahead of the floor.
		// ResolutionDown is a LEVEL command, not an edge: re-issuing it every tick is correct, and
		// FFPMSteerStall is what notices if it is still being re-issued far too long.
		return;
	}

	// Section 3.3: cooldowns key by TIER IDENTITY, not by ladder position. The array is indexed by
	// the enum for exactly that reason -- mode C moves K4f three places and its cooldown follows it.
	CooldownUntil[I] = NowSeconds + TierCooldownSeconds();
}

// ------------------------------------------------------------------------------------------------
// ★ THE DEADLOCK DETECTOR. See FFPMSteerStall's own comment for what fires it and why it cannot fire
// on correct input.
// ------------------------------------------------------------------------------------------------

void FFPMGiveTakeWalk::UpdateStall(const FFPMSteeringInputs& In, const FFPMSteerDecision& Decision)
{
	const bool bOver = FPMSteerGates::MeanOverBudget(In);

	// "No progress" is a blocked decision, OR a ResolutionDown that keeps repeating. Those are the
	// only two shapes that can persist: an engage/demote/release/promote flips a tier's engaged bit on
	// Commit, so it cannot repeat, and a walk that keeps changing the ladder is not stuck.
	const bool bNoProgress = bOver
		&& (!Decision.IsAction() || Decision.Action == EFPMSteerAction::ResolutionDown);

	if (!bNoProgress)
	{
		StallState.bActive = false;
		StallState.Decisions = 0;
		StallState.bReported = false;
		return;
	}

	const bool bNewEpisode = !StallState.bActive
		|| StallState.RepeatedAction != Decision.Action
		|| StallState.RepeatedBlock != Decision.Block;

	if (bNewEpisode)
	{
		StallState.bActive = true;
		StallState.StartedAtSeconds = In.NowSeconds;
		StallState.Decisions = 0;
		StallState.bReported = false;
		++StallState.EpisodesTotal;
	}

	StallState.RepeatedAction = Decision.Action;
	StallState.RepeatedBlock = Decision.Block;
	++StallState.Decisions;

	if (StallState.bReported || (In.NowSeconds - StallState.StartedAtSeconds) < StallWarnSeconds())
	{
		return;
	}
	StallState.bReported = true;

	// EXPECTED stalls are real states the design puts the walk in, and they are reported at Warning so
	// they are visible without crying wolf. Everything else is the SHAPE of the 0.55.0 defect.
	const bool bExpected =
		Decision.Block == EFPMSteerBlock::ModeCarriesNoStageLevers
		|| Decision.Block == EFPMSteerBlock::NoBenchProfile
		|| Decision.Block == EFPMSteerBlock::CpuBound
		|| Decision.Block == EFPMSteerBlock::LadderExhausted;

	if (!bExpected)
	{
		++StallState.EpisodesUnexpected;
	}

	const FString Shape = Decision.IsAction()
		? FString::Printf(TEXT("action '%s' repeating with no progress"), LexToString(Decision.Action))
		: FString::Printf(TEXT("blocked on '%s'"), LexToString(Decision.Block));

	if (bExpected)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] give/take: %.0fs over budget with no ladder progress -- %s. This is an EXPECTED "
			     "state for %s, not a fault. mean %.2fms vs budget %.2fms, %s, at floor=%s. %s"),
			In.NowSeconds - StallState.StartedAtSeconds, *Shape, LexToString(In.Mode),
			In.MeanFrameMs, In.BudgetMs, LexToString(In.Attribution),
			In.bResolutionAtFloor ? TEXT("yes") : TEXT("no"), *Decision.Reason);
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Error,
		TEXT("[FPM] give/take STALL: %.0fs over budget with no ladder progress -- %s, across %d "
		     "decisions. THIS IS THE SHAPE OF THE 0.55.0 DEADLOCK: the ladder is being asked to act "
		     "and is not acting. mean %.2fms vs budget %.2fms, %s, resolution at floor=%s at max=%s, "
		     "profile=%s, mode %s. Reason given: %s"),
		In.NowSeconds - StallState.StartedAtSeconds, *Shape, StallState.Decisions,
		In.MeanFrameMs, In.BudgetMs, LexToString(In.Attribution),
		In.bResolutionAtFloor ? TEXT("yes") : TEXT("no"),
		In.bResolutionAtMax ? TEXT("yes") : TEXT("no"),
		In.bProfileAvailable ? TEXT("yes") : TEXT("no"),
		LexToString(In.Mode), *Decision.Reason);
}

// ------------------------------------------------------------------------------------------------
// ★ THE DRY LADDER WALK. See FFPMGiveTakeWalk::DryRunWalk in the header for what is synthetic and
// why. Nothing below reaches FPMCVarWriter except to READ its held set, twice, as the no-write proof.
// ------------------------------------------------------------------------------------------------

namespace
{
	/**
	 * The declared synthetic starting value for every cvar in the dry walk.
	 *
	 * ⚠ IT IS A CONSTANT AND NOT A LIVE READ, for two separate reasons. A live read makes the walk
	 * non-deterministic, so two runs could not be compared. And a live read may return FPM's own
	 * earlier write, which is the ratchet Law 3 forbids -- the dry walk must not model the very
	 * mistake it exists to rule out.
	 */
	constexpr float FPMDrySyntheticBaseline = 1.0f;

	/** Seconds advanced per dry step. Larger than both dwells (2s and 5s) and larger than the tier
	 *  cooldown (10s), so no step is refused for a timer the dry run is not trying to test. */
	constexpr double FPMDryStepSeconds = 20.0;

	/** The simulated cvar world one dry walk lives in. It exists inside DryRunWalk and nowhere else. */
	struct FFPMDrySimState
	{
		TMap<FString, float> Values;
		int32 GroupTier = 3;
	};
}

void FFPMGiveTakeWalk::DryRunWalk(const EFPMGovernorMode Mode, const int32 MaxSteps,
                                  FFPMDryWalkResult& Out)
{
	Out = FFPMDryWalkResult();
	Out.Mode = Mode;

	const FFPMStageTables& Tables = FFPMStageTables::Get();

	// ---- The no-write proof, first half. ---------------------------------------------------------
	FPMCVarWriter::Get().GetHeldCVars(Out.HeldBefore);

	// ---- Save the live session. A dry run must leave nothing behind. ------------------------------
	bool   SavedEngaged[static_cast<int32>(EFPMStageTier::Count)];
	double SavedCooldown[static_cast<int32>(EFPMStageTier::Count)];
	FMemory::Memcpy(SavedEngaged, bEngaged, sizeof(bEngaged));
	FMemory::Memcpy(SavedCooldown, CooldownUntil, sizeof(CooldownUntil));
	const double SavedOver = OverBudgetSince;
	const double SavedHead = HeadroomSince;
	const FFPMSteerStall SavedStall = StallState;
	const int32 SavedDecisions = DecisionsThisSession;

	FMemory::Memzero(bEngaged, sizeof(bEngaged));
	FMemory::Memzero(CooldownUntil, sizeof(CooldownUntil));
	OverBudgetSince = -1.0;
	HeadroomSince = -1.0;
	StallState = FFPMSteerStall();

	// ---- The simulated cvar world. ---------------------------------------------------------------
	FFPMDrySimState Sim;
	Sim.GroupTier = FFPMStageTables::GroupCeilingTier();
	Out.LowestGITierSeen = Sim.GroupTier;

	for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		for (const FFPMStageLever& Lever : Tables.LeversIn(static_cast<EFPMStageTier>(I)))
		{
			if (!Lever.CVarName.IsEmpty())
			{
				Sim.Values.FindOrAdd(Lever.CVarName) = FPMDrySyntheticBaseline;
			}
		}
	}

	// What a tier held BEFORE it was engaged. A release restores from here and from nowhere else,
	// which is the walk's own statement of Law 3: the restore value is SAVED, never re-read.
	TMap<EFPMStageTier, TMap<FString, float>> SavedByTier;
	TMap<EFPMStageTier, int32> SavedGroupTierByTier;

	FFPMSteeringInputs In;
	In.Mode = Mode;
	In.BudgetMs = 16.667f;
	In.RaiseMs  = 13.333f;
	In.Attribution = EFPMBoundAttribution::Gpu;
	In.bProfileAvailable = true;
	In.BenchNoiseFloorMs = 0.2f;
	In.bResolutionAtFloor = false;
	In.bResolutionAtMax = true;      // vanilla: full resolution, nothing engaged
	for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		In.Measurements[I].bMeasured = true;
		In.Measurements[I].RecoveryMs = 0.8f;
		In.Measurements[I].CostMs = 0.8f;
	}

	// Three phases, because a two-phase series can never show a DEMOTE: a fresh ladder has no bonus
	// engaged to hand back. Climb, then fall, then climb again -- and the third phase must land on the
	// values the first one produced, which is the no-ratchet check.
	const float PhaseMeans[3] = { In.RaiseMs * 0.5f, In.BudgetMs * 1.6f, In.RaiseMs * 0.5f };
	const TCHAR* PhaseNames[3] = { TEXT("headroom (climb)"), TEXT("over budget (fall)"),
	                               TEXT("headroom (recover)") };
	int32 Phase = 0;

	TMap<FString, float> AfterFirstClimb;
	int32 GroupTierAfterFirstClimb = Sim.GroupTier;
	bool bCapturedFirstClimb = false;

	double Now = 0.0;
	int32 StepIndex = 0;

	auto RecordLeverMove = [](FFPMDryWalkStep& Step, const FFPMStageLever& Lever,
	                          const FString& OldText, const FString& NewText, const FString& Note)
	{
		const FString Marker = Lever.AwaitingRuling.IsEmpty()
			? FString()
			: FString::Printf(TEXT("[AWAITING RULING: %s] "), *Lever.AwaitingRuling);
		Step.LeverLines.Add(FString::Printf(TEXT("%-58s %10s -> %-10s %s%s"),
			Lever.GroupName.IsEmpty() ? *Lever.CVarName : *Lever.GroupName,
			*OldText, *NewText, *Marker, *Note));
	};

	while (StepIndex < MaxSteps)
	{
		++StepIndex;
		Now += FPMDryStepSeconds;
		In.NowSeconds = Now;
		In.MeanFrameMs = PhaseMeans[Phase];

		FFPMDryWalkStep Step;
		Step.Step = StepIndex;
		Step.AtSeconds = Now;
		Step.MeanFrameMs = In.MeanFrameMs;
		Step.Decision = Decide(In);

		const EFPMStageTier Tier = Step.Decision.Tier;
		const EFPMSteerAction Action = Step.Decision.Action;

		if (Action == EFPMSteerAction::EngageCut || Action == EFPMSteerAction::PromoteBonus)
		{
			TMap<FString, float>& Saved = SavedByTier.FindOrAdd(Tier);
			Saved.Reset();
			SavedGroupTierByTier.FindOrAdd(Tier) = Sim.GroupTier;

			for (const FFPMStageLever& Lever : Tables.LeversIn(Tier))
			{
				if (!Lever.GroupName.IsEmpty())
				{
					FString GroupNote;
					const int32 Landed = Tables.ResolveGroupTarget(Lever, Sim.GroupTier, GroupNote);
					if (Landed == INDEX_NONE)
					{
						RecordLeverMove(Step, Lever, FString::Printf(TEXT("@%d"), Sim.GroupTier),
							TEXT("(skipped)"), GroupNote);
						continue;
					}
					RecordLeverMove(Step, Lever, FString::Printf(TEXT("@%d"), Sim.GroupTier),
						FString::Printf(TEXT("@%d"), Landed), GroupNote);
					Sim.GroupTier = Landed;
					Out.LowestGITierSeen = FMath::Min(Out.LowestGITierSeen, Sim.GroupTier);
					if (Sim.GroupTier < FFPMStageTables::GIGroupFloorTier())
					{
						Out.bGIFloorHeld = false;
					}
					continue;
				}

				float& Live = Sim.Values.FindOrAdd(Lever.CVarName);
				Saved.Add(Lever.CVarName, Live);
				FString ProjNote;
				const float Landed = FPMProjectLeverValue(Lever, Live, ProjNote);
				RecordLeverMove(Step, Lever, FPMFormatLeverValue(Live), FPMFormatLeverValue(Landed),
					ProjNote.IsEmpty() ? Lever.Note : ProjNote);
				Live = Landed;
			}
		}
		else if (Action == EFPMSteerAction::ReleaseCut || Action == EFPMSteerAction::DemoteBonus)
		{
			if (const int32* SavedGroup = SavedGroupTierByTier.Find(Tier))
			{
				for (const FFPMStageLever& Lever : Tables.LeversIn(Tier))
				{
					if (Lever.GroupName.IsEmpty()) { continue; }
					RecordLeverMove(Step, Lever, FString::Printf(TEXT("@%d"), Sim.GroupTier),
						FString::Printf(TEXT("@%d"), *SavedGroup),
						TEXT("restored from the value SAVED before the engage, never re-read"));
					Sim.GroupTier = *SavedGroup;
				}
			}
			if (const TMap<FString, float>* Saved = SavedByTier.Find(Tier))
			{
				for (const FFPMStageLever& Lever : Tables.LeversIn(Tier))
				{
					if (Lever.GroupName.IsEmpty() && Saved->Contains(Lever.CVarName))
					{
						float& Live = Sim.Values.FindOrAdd(Lever.CVarName);
						const float Restore = (*Saved)[Lever.CVarName];
						RecordLeverMove(Step, Lever, FPMFormatLeverValue(Live),
							FPMFormatLeverValue(Restore),
							TEXT("restored from the value SAVED before the engage, never re-read"));
						Live = Restore;
					}
				}
			}
		}
		else if (Action == EFPMSteerAction::ResolutionDown)
		{
			Step.LeverLines.Add(TEXT("(no stage levers -- section 8 owns resolution. The dry run "
			                         "models a PERFECT executor: the floor is reached in one step.)"));
			In.bResolutionAtFloor = true;
			In.bResolutionAtMax = false;
		}
		else if (Action == EFPMSteerAction::ResolutionUp)
		{
			Step.LeverLines.Add(TEXT("(no stage levers -- section 8 owns resolution. The dry run "
			                         "models a PERFECT executor: the maximum is reached in one step.)"));
			In.bResolutionAtMax = true;
			In.bResolutionAtFloor = false;
		}

		if (Step.Decision.IsAction())
		{
			Commit(Step.Decision, Now);
		}

		Out.Steps.Add(MoveTemp(Step));

		// A block that is not the dwell means this phase has nothing left to do. The dwell is
		// transient by construction: the next step is 20 seconds later and clears it.
		const bool bPhaseSettled = !Out.Steps.Last().Decision.IsAction()
			&& Out.Steps.Last().Decision.Block != EFPMSteerBlock::Dwell;

		if (!bPhaseSettled) { continue; }

		if (Phase == 0 && !bCapturedFirstClimb)
		{
			AfterFirstClimb = Sim.Values;
			GroupTierAfterFirstClimb = Sim.GroupTier;
			bCapturedFirstClimb = true;
		}
		if (Phase == 2)
		{
			Out.bConverged = true;
			Out.StopReason = FString::Printf(
				TEXT("all three phases settled in %d step(s). Last block: %s"),
				StepIndex, LexToString(Out.Steps.Last().Decision.Block));
			break;
		}
		++Phase;
		// A phase change is a new signal regime, so the dwell state must start again rather than
		// carry a stale timestamp from the phase before.
		OverBudgetSince = -1.0;
		HeadroomSince = -1.0;
	}

	if (!Out.bConverged)
	{
		Out.StopReason = FString::Printf(
			TEXT("STEP BUDGET of %d exhausted in phase %d (%s). This is NOT a pass: the ladder did not "
			     "settle."), MaxSteps, Phase, PhaseNames[FMath::Clamp(Phase, 0, 2)]);
	}

	// ---- THE NO-RATCHET CHECK. Climb, fall, climb must land where the first climb left it. --------
	if (bCapturedFirstClimb)
	{
		FString Delta;
		for (const TPair<FString, float>& Pair : AfterFirstClimb)
		{
			const float* Nowv = Sim.Values.Find(Pair.Key);
			if (!Nowv || !FMath::IsNearlyEqual(*Nowv, Pair.Value, 1.e-4f))
			{
				Delta += FString::Printf(TEXT("%s %s->%s "), *Pair.Key,
					*FPMFormatLeverValue(Pair.Value),
					Nowv ? *FPMFormatLeverValue(*Nowv) : TEXT("(gone)"));
			}
		}
		if (Sim.GroupTier != GroupTierAfterFirstClimb)
		{
			Delta += FString::Printf(TEXT("GI group @%d->@%d "), GroupTierAfterFirstClimb, Sim.GroupTier);
		}
		Out.bReturnedToStart = Delta.IsEmpty();
		Out.RatchetDelta = Delta;
	}
	else
	{
		Out.bReturnedToStart = false;
		Out.RatchetDelta = TEXT("the first climb never settled, so there is no reference state to "
		                        "compare the recovery against");
	}

	// ---- Restore the live session, then the no-write proof's second half. -------------------------
	FMemory::Memcpy(bEngaged, SavedEngaged, sizeof(bEngaged));
	FMemory::Memcpy(CooldownUntil, SavedCooldown, sizeof(CooldownUntil));
	OverBudgetSince = SavedOver;
	HeadroomSince = SavedHead;
	StallState = SavedStall;
	DecisionsThisSession = SavedDecisions;

	FPMCVarWriter::Get().GetHeldCVars(Out.HeldAfter);
	Out.bHeldSetIdentical = FPMStageInvariants::SameNameSet(Out.HeldBefore, Out.HeldAfter,
		Out.HeldSetDelta);
}

void FFPMGiveTakeWalk::PrintDryRun(const FFPMDryWalkResult& Result, FOutputDevice& Ar)
{
	FPMScopedConsoleEcho Echo(&Ar);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] DRY LADDER WALK -- mode %s, %d step(s). %s"),
		LexToString(Result.Mode), Result.Steps.Num(), *Result.StopReason);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   SYNTHETIC: the signal series, the per-tier measurements (uniform 0.8ms), the "
		     "starting cvar values (%s for every lever, a declared constant and NOT a live read), and "
		     "a PERFECT resolution executor. Nothing here is a measurement."),
		*FPMFormatLeverValue(FPMDrySyntheticBaseline));

	for (const FFPMDryWalkStep& Step : Result.Steps)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %3d  t=%6.0fs  mean %6.2fms  %-16s %-4s %-28s %s"),
			Step.Step, Step.AtSeconds, Step.MeanFrameMs,
			LexToString(Step.Decision.Action), LexToString(Step.Decision.Tier),
			LexToString(Step.Decision.Block), *Step.Decision.Reason);
		for (const FString& Line : Step.LeverLines)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]          %s"), *Line);
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   NO-WRITE PROOF: FPMCVarWriter held %d cvar(s) before and %d after, identical=%s. "
		     "%s"),
		Result.HeldBefore.Num(), Result.HeldAfter.Num(),
		Result.bHeldSetIdentical ? TEXT("yes") : TEXT("NO"),
		Result.bHeldSetIdentical ? TEXT("The walk resolved and printed; it applied nothing.")
		                         : *Result.HeldSetDelta);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   NO-RATCHET PROOF: climb, fall, climb returned to the first climb's values=%s. %s"),
		Result.bReturnedToStart ? TEXT("yes") : TEXT("NO"),
		Result.bReturnedToStart
			? TEXT("Every release restored the value SAVED before its engage, so a repeated cycle "
			       "cannot walk the baseline anywhere.")
			: *Result.RatchetDelta);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   GI FLOOR: lowest GlobalIlluminationQuality tier reached was @%d against a floor "
		     "of @%d, held=%s."),
		Result.LowestGITierSeen, FFPMStageTables::GIGroupFloorTier(),
		Result.bGIFloorHeld ? TEXT("yes") : TEXT("NO"));
}

// ------------------------------------------------------------------------------------------------
// ★ THE SELF-TEST. Runs at world load, after the stage tables' own self-test, so the orders it walks
// are already proven internally consistent.
// ------------------------------------------------------------------------------------------------

bool FFPMGiveTakeWalk::SelfTest()
{
	bool bOk = true;

	// Save and restore the live ladder. The grid below drives hundreds of decisions and must not leave
	// the walk believing anything about the real session.
	bool SavedEngaged[static_cast<int32>(EFPMStageTier::Count)];
	FMemory::Memcpy(SavedEngaged, bEngaged, sizeof(bEngaged));
	const double SavedOver = OverBudgetSince;
	const double SavedHead = HeadroomSince;
	const FFPMSteerStall SavedStall = StallState;
	// The grid below drives well over a thousand decisions. Leaving them in the session counter would
	// make the report print a number that describes this test rather than the session.
	const int32 SavedDecisions = DecisionsThisSession;

	const float Budget = 16.667f;   // 60 fps target
	const float Raise  = 13.333f;   // TargetFPS + 15, the design's dead-band spread

	auto FreshInputs = [Budget, Raise](const EFPMGovernorMode Mode)
	{
		FFPMSteeringInputs In;
		In.Mode = Mode;
		In.BudgetMs = Budget;
		In.RaiseMs = Raise;
		In.NowSeconds = 0.0;
		return In;
	};

	// Runs one case past the dwell without leaving dwell state behind for the next.
	auto DecidePastDwell = [this](FFPMSteeringInputs In)
	{
		OverBudgetSince = -1.0;
		HeadroomSince = -1.0;
		StallState = FFPMSteerStall();
		In.NowSeconds = 0.0;
		Decide(In);
		In.NowSeconds = 1000.0;
		return Decide(In);
	};

	// ---- (1) NO SILENT NO-OP, across a grid of reachable states. ---------------------------------
	{
		const EFPMGovernorMode Modes[] = {
			EFPMGovernorMode::Balanced, EFPMGovernorMode::ResolutionFirst,
			EFPMGovernorMode::GraphicsFirst, EFPMGovernorMode::LightingFirst };
		const float Means[] = { Budget * 1.5f, (Budget + Raise) * 0.5f, Raise * 0.5f };
		const EFPMBoundAttribution Attributions[] = {
			EFPMBoundAttribution::Gpu, EFPMBoundAttribution::Cpu, EFPMBoundAttribution::Unknown };

		int32 Cases = 0;
		int32 Silent = 0;
		for (const EFPMGovernorMode Mode : Modes)
		{
			for (const float Mean : Means)
			{
				for (const EFPMBoundAttribution Attribution : Attributions)
				{
					for (int32 Floor = 0; Floor < 2; ++Floor)
					{
						for (int32 Max = 0; Max < 2; ++Max)
						{
							for (int32 Profile = 0; Profile < 2; ++Profile)
							{
								for (int32 Ladder = 0; Ladder < 3; ++Ladder)
								{
									FMemory::Memzero(bEngaged, sizeof(bEngaged));
									for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
									{
										const EFPMStageTier Tier = static_cast<EFPMStageTier>(I);
										if (Ladder == 1 && FPMIsBonusTier(Tier)) { bEngaged[I] = true; }
										if (Ladder == 2 && FPMIsCutTier(Tier))   { bEngaged[I] = true; }
									}

									FFPMSteeringInputs In = FreshInputs(Mode);
									In.MeanFrameMs = Mean;
									In.Attribution = Attribution;
									In.bResolutionAtFloor = Floor != 0;
									In.bResolutionAtMax = Max != 0;
									In.bProfileAvailable = Profile != 0;
									In.BenchNoiseFloorMs = 0.2f;
									// A plausible profile: every tier measured, small numbers.
									for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
									{
										In.Measurements[I].bMeasured = Profile != 0;
										In.Measurements[I].RecoveryMs = 0.8f;
										In.Measurements[I].CostMs = 0.8f;
									}

									const FFPMSteerDecision D = DecidePastDwell(In);
									++Cases;
									if (!D.IsAction() && D.Block == EFPMSteerBlock::None)
									{
										++Silent;
										if (Silent <= 3)
										{
											UE_LOG(LogFicsitsPerformanceManager, Error,
												TEXT("[FPM] give/take self-test (1): SILENT no-op -- "
												     "mode %s, mean %.2f, %s, floor=%d max=%d "
												     "profile=%d ladder=%d"),
												LexToString(Mode), Mean, LexToString(Attribution),
												Floor, Max, Profile, Ladder);
										}
									}
									if (D.Reason.IsEmpty())
									{
										UE_LOG(LogFicsitsPerformanceManager, Error,
											TEXT("[FPM] give/take self-test (1): a decision carried no "
											     "reason (mode %s, action %s, block %s)"),
											LexToString(Mode), LexToString(D.Action), LexToString(D.Block));
										bOk = false;
									}
								}
							}
						}
					}
				}
			}
		}
		if (Silent > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (1) FAILED: %d of %d reachable states returned neither "
				     "an action nor a named block. That is the exact failure the 0.55.0 ladder had."),
				Silent, Cases);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] give/take self-test (1) passed: %d reachable states, every one returned an "
				     "action or a named block."), Cases);
		}
	}

	// ---- (2) THE 0.55.0 REGRESSION, on its own numbers, in both directions. ----------------------
	// Her resolution ran 67, 58, 50, 58: mid-band, touching neither end. Under the old controller that
	// state produced NOTHING in either direction. Here it must produce an action both ways.
	{
		FMemory::Memzero(bEngaged, sizeof(bEngaged));
		FFPMSteeringInputs In = FreshInputs(EFPMGovernorMode::ResolutionFirst);
		In.MeanFrameMs = 20.0f;                 // about 50 fps against a 60 fps target
		In.Attribution = EFPMBoundAttribution::Gpu;
		In.bResolutionAtFloor = false;          // 58%, above the floor
		In.bResolutionAtMax = false;            // 58%, below full
		In.bProfileAvailable = true;
		In.BenchNoiseFloorMs = 0.2f;
		for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
		{
			In.Measurements[I].bMeasured = true;
			In.Measurements[I].RecoveryMs = 0.8f;
			In.Measurements[I].CostMs = 0.8f;
		}

		const FFPMSteerDecision Down = DecidePastDwell(In);
		if (Down.Action != EFPMSteerAction::ResolutionDown)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (2a) FAILED: over budget, GPU-bound, resolution "
				     "mid-band -- expected resolution-down, got action '%s' block '%s'. This is the "
				     "'too high to cut' half of the 0.55.0 deadlock."),
				LexToString(Down.Action), LexToString(Down.Block));
			bOk = false;
		}

		In.MeanFrameMs = 8.0f;                  // clear headroom, still mid-band resolution
		const FFPMSteerDecision Up = DecidePastDwell(In);
		if (!Up.IsAction())
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (2b) FAILED: clear headroom with resolution mid-band "
				     "produced no action (block '%s'). This is the 'too low to boost' half of the "
				     "0.55.0 deadlock."), LexToString(Up.Block));
			bOk = false;
		}
	}

	// ---- (3) BENCH-WORTHWHILE, both directions AND the large-miss case the delta exists for. ------
	{
		FFPMSteeringInputs In = FreshInputs(EFPMGovernorMode::GraphicsFirst);
		In.BenchNoiseFloorMs = 0.2f;
		const int32 K1 = static_cast<int32>(EFPMStageTier::K1);
		FString Why;

		// (3a) Small miss, healthy recovery: worthwhile.
		In.MeanFrameMs = 17.5f;                        // overshoot 0.833ms, 25% = 0.208ms
		In.Measurements[K1] = { true, 0.4f, 0.4f };
		const bool bSmallMiss = FPMSteerGates::BenchWorthwhile(In, EFPMStageTier::K1, 1.4f, Why);

		// (3b) Recovery under the noise floor: refused, whatever the miss looks like.
		In.Measurements[K1] = { true, 0.05f, 0.05f };
		const bool bUnderNoise = FPMSteerGates::BenchWorthwhile(In, EFPMStageTier::K1, 1.4f, Why);

		// (3c) THE LARGE MISS. 30 fps against a 60 fps target: overshoot 16.6ms, so the 25% term wants
		// 4.16ms from a ladder whose largest rung is K3 at about 1.4ms. The design as written refuses
		// every tier here; the delta must let it through.
		In.MeanFrameMs = 33.3f;
		In.Measurements[K1] = { true, 0.4f, 0.4f };
		const bool bLargeMiss = FPMSteerGates::BenchWorthwhile(In, EFPMStageTier::K1, 1.4f, Why);

		// (3d) The delta must NOT swallow the noise floor even on a large miss.
		In.Measurements[K1] = { true, 0.05f, 0.05f };
		const bool bLargeMissUnderNoise = FPMSteerGates::BenchWorthwhile(In, EFPMStageTier::K1, 1.4f, Why);

		// (3e) An unmeasured tier is never worthwhile. Unproven means it does not move.
		In.Measurements[K1] = FFPMTierMeasurement();
		const bool bUnmeasured = FPMSteerGates::BenchWorthwhile(In, EFPMStageTier::K1, 1.4f, Why);

		if (!bSmallMiss || bUnderNoise || !bLargeMiss || bLargeMissUnderNoise || bUnmeasured)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (3) FAILED: small-miss worthwhile=%s (want yes), "
				     "under-noise=%s (want no), LARGE-miss worthwhile=%s (want yes, this is the design "
				     "delta), large-miss-under-noise=%s (want no), unmeasured=%s (want no)."),
				bSmallMiss ? TEXT("yes") : TEXT("no"), bUnderNoise ? TEXT("yes") : TEXT("no"),
				bLargeMiss ? TEXT("yes") : TEXT("no"),
				bLargeMissUnderNoise ? TEXT("yes") : TEXT("no"),
				bUnmeasured ? TEXT("yes") : TEXT("no"));
			bOk = false;
		}
	}

	// ---- (4) SATURATION MUST NOT STRAND THE RESOLUTION RESTORE. ----------------------------------
	// The mirror of the promote-side bug section 3.3 names. With every bonus tier unpromotable (here:
	// measured and far too expensive for the headroom), quality IS saturated and mode B must reach its
	// resolution step. With every bonus cheap, it must NOT -- it should promote instead.
	{
		FFPMSteeringInputs In = FreshInputs(EFPMGovernorMode::GraphicsFirst);
		In.MeanFrameMs = 8.0f;                  // clear headroom of 5.333ms
		In.Attribution = EFPMBoundAttribution::Gpu;
		In.bResolutionAtFloor = true;
		In.bResolutionAtMax = false;
		In.bProfileAvailable = true;
		In.BenchNoiseFloorMs = 0.2f;

		FMemory::Memzero(bEngaged, sizeof(bEngaged));
		for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
		{
			In.Measurements[I] = { true, 0.8f, 999.0f };   // every bonus unaffordable
		}
		const FFPMSteerDecision Stranded = DecidePastDwell(In);

		for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
		{
			In.Measurements[I] = { true, 0.8f, 0.1f };     // every bonus cheap
		}
		FMemory::Memzero(bEngaged, sizeof(bEngaged));
		const FFPMSteerDecision Promotes = DecidePastDwell(In);

		if (Stranded.Action != EFPMSteerAction::ResolutionUp
			|| Promotes.Action != EFPMSteerAction::PromoteBonus)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (4) FAILED: with every bonus unaffordable the take side "
				     "gave '%s' (want resolution-up, because a bonus that cannot be promoted must not "
				     "strand R+); with every bonus cheap it gave '%s' (want promote-bonus)."),
				LexToString(Stranded.Action), LexToString(Promotes.Action));
			bOk = false;
		}
	}

	// ---- (5) INVARIANT: THE WALK IS IDEMPOTENT WHEN THE INPUT DOES NOT CHANGE. -------------------
	// Two Decide calls on a byte-identical FFPMSteeringInputs, with no Commit between them, must
	// return the same action, the same tier and the same block. A walk that answered differently to
	// the same question could not be reasoned about at all, and a caller retrying after a failed
	// apply would get a different lever the second time.
	{
		const EFPMGovernorMode Modes[] = {
			EFPMGovernorMode::Balanced, EFPMGovernorMode::ResolutionFirst,
			EFPMGovernorMode::GraphicsFirst, EFPMGovernorMode::LightingFirst };
		const float Means[] = { Budget * 1.5f, (Budget + Raise) * 0.5f, Raise * 0.5f };

		int32 Pairs = 0;
		int32 Divergent = 0;
		for (const EFPMGovernorMode Mode : Modes)
		{
			for (const float Mean : Means)
			{
				for (int32 Floor = 0; Floor < 2; ++Floor)
				{
					FMemory::Memzero(bEngaged, sizeof(bEngaged));
					FFPMSteeringInputs In = FreshInputs(Mode);
					In.MeanFrameMs = Mean;
					In.Attribution = EFPMBoundAttribution::Gpu;
					In.bResolutionAtFloor = Floor != 0;
					In.bProfileAvailable = true;
					In.BenchNoiseFloorMs = 0.2f;
					for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
					{
						In.Measurements[I] = { true, 0.8f, 0.8f };
					}

					const FFPMSteerDecision First = DecidePastDwell(In);
					// Same clock, same signal, no Commit. The only thing that could differ is the
					// walk's own bookkeeping, and it must not.
					In.NowSeconds = 1000.0;
					const FFPMSteerDecision Second = Decide(In);
					++Pairs;

					if (First.Action != Second.Action || First.Tier != Second.Tier
						|| First.Block != Second.Block)
					{
						++Divergent;
						if (Divergent <= 3)
						{
							UE_LOG(LogFicsitsPerformanceManager, Error,
								TEXT("[FPM] give/take self-test (5): NOT IDEMPOTENT -- mode %s mean "
								     "%.2f floor=%d gave '%s'/'%s'/'%s' then '%s'/'%s'/'%s'"),
								LexToString(Mode), Mean, Floor,
								LexToString(First.Action), LexToString(First.Tier),
								LexToString(First.Block), LexToString(Second.Action),
								LexToString(Second.Tier), LexToString(Second.Block));
						}
					}
				}
			}
		}
		if (Divergent > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (5) FAILED: %d of %d repeated decisions differed on "
				     "identical input."), Divergent, Pairs);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] give/take self-test (5) passed: %d repeated decisions, every one identical "
				     "on identical input."), Pairs);
		}
	}

	// ---- (6) THE DRY LADDER WALK, ALL THREE STEERABLE MODES, WITH ITS OWN THREE PROOFS. ----------
	// This is the check that exercises the 3071 lines of tier content end to end without a boot: the
	// walk must converge, it must write nothing, it must not ratchet, and it must never take the GI
	// group below its floor.
	{
		const EFPMGovernorMode WalkModes[] = {
			EFPMGovernorMode::ResolutionFirst, EFPMGovernorMode::GraphicsFirst,
			EFPMGovernorMode::LightingFirst };

		for (const EFPMGovernorMode Mode : WalkModes)
		{
			FFPMDryWalkResult Result;
			DryRunWalk(Mode, 300, Result);

			if (!Result.bConverged || !Result.bHeldSetIdentical || !Result.bReturnedToStart
				|| !Result.bGIFloorHeld)
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] give/take self-test (6) FAILED for mode %s: converged=%s (%s), "
					     "no-write=%s (%s), no-ratchet=%s (%s), GI floor held=%s (lowest @%d)."),
					LexToString(Mode),
					Result.bConverged ? TEXT("yes") : TEXT("NO"), *Result.StopReason,
					Result.bHeldSetIdentical ? TEXT("yes") : TEXT("NO"), *Result.HeldSetDelta,
					Result.bReturnedToStart ? TEXT("yes") : TEXT("NO"), *Result.RatchetDelta,
					Result.bGIFloorHeld ? TEXT("yes") : TEXT("NO"), Result.LowestGITierSeen);
				bOk = false;
			}
			else
			{
				int32 Gives = 0;
				int32 Takes = 0;
				for (const FFPMDryWalkStep& Step : Result.Steps)
				{
					switch (Step.Decision.Action)
					{
					case EFPMSteerAction::DemoteBonus:
					case EFPMSteerAction::EngageCut:
					case EFPMSteerAction::ResolutionDown: ++Gives; break;
					case EFPMSteerAction::ReleaseCut:
					case EFPMSteerAction::PromoteBonus:
					case EFPMSteerAction::ResolutionUp:   ++Takes; break;
					default: break;
					}
				}
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] give/take self-test (6) passed for mode %s: %d step(s), %d give(s), "
					     "%d take(s), held cvars %d before and %d after, lowest GI tier @%d."),
					LexToString(Mode), Result.Steps.Num(), Gives, Takes,
					Result.HeldBefore.Num(), Result.HeldAfter.Num(), Result.LowestGITierSeen);

				// ★ A WALK THAT MOVED NOTHING WOULD PASS ALL THREE PROOFS TRIVIALLY. That is the dead
				// instrument shape, so a zero here is a failure and not a clean bill.
				if (Gives == 0 || Takes == 0)
				{
					UE_LOG(LogFicsitsPerformanceManager, Error,
						TEXT("[FPM] give/take self-test (6) FAILED for mode %s: the walk converged "
						     "with %d give(s) and %d take(s). A walk that moved nothing satisfies "
						     "every proof above without proving anything."),
						LexToString(Mode), Gives, Takes);
					bOk = false;
				}
			}
		}

		// THE NO-WRITE COMPARATOR'S OWN LIVENESS. It must be able to say NO, or every "identical"
		// verdict above is worth nothing. Feed it a perturbed copy of a real snapshot.
		TArray<FString> Held;
		FPMCVarWriter::Get().GetHeldCVars(Held);
		TArray<FString> Perturbed = Held;
		Perturbed.Add(TEXT("__SelfTest.CVarThatIsNotHeld"));
		FString SameDelta;
		FString DiffDelta;
		const bool bSaysSame = FPMStageInvariants::SameNameSet(Held, Held, SameDelta);
		const bool bSaysDifferent = !FPMStageInvariants::SameNameSet(Held, Perturbed, DiffDelta);
		if (!bSaysSame || !bSaysDifferent)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] give/take self-test (6) FAILED: the held-cvar comparator called an "
				     "identical pair %s (want identical) and a perturbed pair %s (want different). "
				     "A comparator that cannot say NO turns every no-write proof into a formality."),
				bSaysSame ? TEXT("identical") : TEXT("DIFFERENT"),
				bSaysDifferent ? TEXT("different") : TEXT("IDENTICAL"));
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] give/take self-test (6) comparator liveness passed: it reported the "
				     "perturbed pair as different with '%s'."), *DiffDelta);
		}
	}

	FMemory::Memcpy(bEngaged, SavedEngaged, sizeof(bEngaged));
	OverBudgetSince = SavedOver;
	HeadroomSince = SavedHead;
	StallState = SavedStall;
	DecisionsThisSession = SavedDecisions;
	return bOk;
}

// ------------------------------------------------------------------------------------------------
// IFPMFix
// ------------------------------------------------------------------------------------------------

void FFPMGiveTakeWalk::Arm()
{
	FMemory::Memzero(bEngaged, sizeof(bEngaged));
	FMemory::Memzero(CooldownUntil, sizeof(CooldownUntil));
	OverBudgetSince = -1.0;
	HeadroomSince = -1.0;
	StallState = FFPMSteerStall();
	DecisionsThisSession = 0;

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] give/take walk armed. The apply pass that executes a decision IS built "
		     "(FPM.Stage.Apply). What still does not exist is a DRIVER: the signals that fill these "
		     "inputs (section 3.6) and the bench that fills the per-tier measurements (section 4) are "
		     "separate items, and without a bench profile every stage tier is refused by design. "
		     "FPM.Stage.Simulate exercises the decision by hand; the self-test runs at world load."));
}

void FFPMGiveTakeWalk::OnWorldLoad(UWorld* World)
{
	bSelfTestPassed = SelfTest();

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] give/take walk: self-test %s."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

void FFPMGiveTakeWalk::Disarm()
{
	FMemory::Memzero(bEngaged, sizeof(bEngaged));
	FMemory::Memzero(CooldownUntil, sizeof(CooldownUntil));
	OverBudgetSince = -1.0;
	HeadroomSince = -1.0;
	StallState = FFPMSteerStall();
	bSelfTestPassed = false;
	DecisionsThisSession = 0;
}

// ------------------------------------------------------------------------------------------------
// Reports.
// ------------------------------------------------------------------------------------------------

void FFPMGiveTakeWalk::ReportNow(FOutputDevice& Ar) const
{
	FPMScopedConsoleEcho Echo(&Ar);

	if (!bSelfTestPassed)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] give/take: the self-test has not passed (it runs at world load). REFUSING to "
			     "print a ladder position produced by an unproven walk."));
		return;
	}

	TArray<EFPMStageTier> Engaged;
	GetEngaged(Engaged);
	FString EngagedLine;
	for (const EFPMStageTier Tier : Engaged)
	{
		EngagedLine += FString::Printf(TEXT("%s "), LexToString(Tier));
	}
	if (EngagedLine.IsEmpty()) { EngagedLine = TEXT("(nothing engaged)"); }

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] give/take -- %d decision(s) this session. Ladder: %s"),
		DecisionsThisSession, *EngagedLine);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   dwells: give %.1fs, take %.1fs. Tier cooldown %.1fs. Stall warning at %.0fs. "
		     "All four are [DEFAULT] and none has a mover yet; the bench is the intended one for the "
		     "dwells (section 4.6)."),
		GiveDwellSeconds(), TakeDwellSeconds(), TierCooldownSeconds(), StallWarnSeconds());

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   stall ledger: %d episode(s), %d of them UNEXPECTED. Currently %s%s"),
		StallState.EpisodesTotal, StallState.EpisodesUnexpected,
		StallState.bActive ? TEXT("ACTIVE, ") : TEXT("clear"),
		StallState.bActive
			? *FString::Printf(TEXT("%d decision(s) on action '%s' / block '%s'"),
				StallState.Decisions, LexToString(StallState.RepeatedAction),
				LexToString(StallState.RepeatedBlock))
			: TEXT(""));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   NOTHING DRIVES THIS WALK YET. The decision count above is whatever "
		     "FPM.Stage.Simulate and the self-test produced, never live steering: the signals and the "
		     "bench are separate items. The apply pass exists -- FPM.Stage.Apply.Report says what a "
		     "driver is still waiting for."));
}

void FFPMGiveTakeWalk::SimulateAndPrint(const TArray<FString>& Args, FOutputDevice& Ar)
{
	FPMScopedConsoleEcho Echo(&Ar);

	// ---- THE MULTI-STEP DRY WALK. Same command, because a second console command over the same
	// ---- inputs is a second thing to keep in step with this one.
	if (Args.Num() >= 1 && Args[0].ToLower() == TEXT("walk"))
	{
		const FString WalkMode = Args.Num() > 1 ? Args[1].ToLower() : FString(TEXT("all"));
		const int32 MaxSteps = Args.Num() > 2 ? FMath::Max(1, FCString::Atoi(*Args[2])) : 300;

		TArray<EFPMGovernorMode> Modes;
		if (WalkMode == TEXT("a") || WalkMode == TEXT("resolution")) { Modes.Add(EFPMGovernorMode::ResolutionFirst); }
		else if (WalkMode == TEXT("b") || WalkMode == TEXT("graphics")) { Modes.Add(EFPMGovernorMode::GraphicsFirst); }
		else if (WalkMode == TEXT("c") || WalkMode == TEXT("lighting")) { Modes.Add(EFPMGovernorMode::LightingFirst); }
		else if (WalkMode == TEXT("balanced")) { Modes.Add(EFPMGovernorMode::Balanced); }
		else
		{
			Modes.Add(EFPMGovernorMode::ResolutionFirst);
			Modes.Add(EFPMGovernorMode::GraphicsFirst);
			Modes.Add(EFPMGovernorMode::LightingFirst);
		}

		for (const EFPMGovernorMode Mode : Modes)
		{
			FFPMDryWalkResult Result;
			DryRunWalk(Mode, MaxSteps, Result);
			PrintDryRun(Result, Ar);
		}

		// The owed rulings, printed with the walk, because a reader who has just watched the ladder
		// move is exactly the reader who needs to know which parts of it are not settled.
		TArray<FString> Rulings;
		FFPMStageTables::Get().GetAwaitingRulings(Rulings);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   AWAITING RULING -- %d lever(s) below, plus two rulings that are not "
			     "per-lever: K4g derives to the empty set on this engine build, and the "
			     "bench-worthwhile second disjunct is a delta from the design as written."),
			Rulings.Num());
		for (const FString& Line : Rulings)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]        %s"), *Line);
		}
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]        AWAITING RULING [K4g-empty] %s"),
			*FFPMStageTables::Get().GetK4gDerivationNote());
		return;
	}

	if (Args.Num() < 2)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] usage: FPM.Stage.Simulate <balanced|a|b|c> <meanMs> [budgetMs=16.667] "
			     "[gpu|cpu|unknown] [atFloor 0|1] [atMax 0|1] [profile 0|1]"));
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]    or: FPM.Stage.Simulate walk [a|b|c|balanced|all] [maxSteps=300] -- the full "
			     "dry ladder walk. Applies nothing and leaves the ladder untouched."));
		return;
	}

	FFPMSteeringInputs In;
	const FString ModeArg = Args[0].ToLower();
	if (ModeArg == TEXT("a") || ModeArg == TEXT("resolution")) { In.Mode = EFPMGovernorMode::ResolutionFirst; }
	else if (ModeArg == TEXT("b") || ModeArg == TEXT("graphics")) { In.Mode = EFPMGovernorMode::GraphicsFirst; }
	else if (ModeArg == TEXT("c") || ModeArg == TEXT("lighting")) { In.Mode = EFPMGovernorMode::LightingFirst; }
	else { In.Mode = EFPMGovernorMode::Balanced; }

	In.MeanFrameMs = FCString::Atof(*Args[1]);
	In.BudgetMs = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 16.667f;
	In.RaiseMs = In.BudgetMs * 0.8f;

	if (Args.Num() > 3)
	{
		const FString A = Args[3].ToLower();
		In.Attribution = (A == TEXT("gpu")) ? EFPMBoundAttribution::Gpu
			: (A == TEXT("cpu")) ? EFPMBoundAttribution::Cpu : EFPMBoundAttribution::Unknown;
	}
	In.bResolutionAtFloor = Args.Num() > 4 && Args[4] == TEXT("1");
	In.bResolutionAtMax   = Args.Num() > 5 && Args[5] == TEXT("1");
	In.bProfileAvailable  = Args.Num() > 6 && Args[6] == TEXT("1");
	In.BenchNoiseFloorMs = 0.2f;
	for (int32 I = 0; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		In.Measurements[I].bMeasured = In.bProfileAvailable;
		In.Measurements[I].RecoveryMs = 0.8f;
		In.Measurements[I].CostMs = 0.8f;
	}

	// Every gate term printed on its own line. A term nobody can see the value of is a term nobody can
	// tell is dead, which is the whole lesson of the ladder that never engaged.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] simulate: mode %s, mean %.3fms, budget %.3fms, raise %.3fms, %s, at floor=%s, "
		     "at max=%s, profile=%s (synthetic: every tier measured at 0.8ms recovery/cost)"),
		LexToString(In.Mode), In.MeanFrameMs, In.BudgetMs, In.RaiseMs, LexToString(In.Attribution),
		In.bResolutionAtFloor ? TEXT("yes") : TEXT("no"),
		In.bResolutionAtMax ? TEXT("yes") : TEXT("no"),
		In.bProfileAvailable ? TEXT("yes") : TEXT("no"));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   gates: over-budget=%s, has-headroom=%s, overshoot=%.3fms, gpu-bound=%s, "
		     "quality-saturated=%s, best-remaining-recovery=%.3fms"),
		FPMSteerGates::MeanOverBudget(In) ? TEXT("yes") : TEXT("no"),
		FPMSteerGates::MeanHasHeadroom(In) ? TEXT("yes") : TEXT("no"),
		FPMSteerGates::OvershootMs(In),
		FPMSteerGates::GpuBound(In) ? TEXT("yes") : TEXT("no"),
		IsQualitySaturated(In) ? TEXT("yes") : TEXT("no"),
		BestRemainingRecoveryMs(In));

	// Drive it past the dwell so the answer is the steady-state one rather than "still dwelling", and
	// leave the live dwell/stall state exactly as it was found.
	const double SavedOver = OverBudgetSince;
	const double SavedHead = HeadroomSince;
	const FFPMSteerStall SavedStall = StallState;
	OverBudgetSince = -1.0;
	HeadroomSince = -1.0;
	StallState = FFPMSteerStall();

	In.NowSeconds = 0.0;
	Decide(In);
	In.NowSeconds = 1000.0;
	const FFPMSteerDecision Decision = Decide(In);

	OverBudgetSince = SavedOver;
	HeadroomSince = SavedHead;
	StallState = SavedStall;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   DECISION: action '%s' tier '%s' block '%s'%s"),
		LexToString(Decision.Action), LexToString(Decision.Tier), LexToString(Decision.Block),
		Decision.bResolutionSlewConcurrent ? TEXT(" (+ concurrent resolution slew)") : TEXT(""));
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   WHY: %s"), *Decision.Reason);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   Nothing was applied by THIS command. It decides only. FPM.Stage.Apply is the "
		     "pass that writes a decision, and it is a separate, deliberate keystroke."));
}

static FAutoConsoleCommandWithOutputDevice GFPMWalkReportCmd(
	TEXT("FPM.Stage.Walk"),
	TEXT("Give/take walk: ladder position, dwells, cooldowns and the stall ledger. Reads only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMGiveTakeWalk::Get().ReportNow(Ar);
	}));

static FAutoConsoleCommandWithArgsAndOutputDevice GFPMWalkSimulateCmd(
	TEXT("FPM.Stage.Simulate"),
	TEXT("Run ONE give/take decision against injected signal and print every gate term with it. "
	     "Applies nothing and leaves the ladder untouched."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			FFPMGiveTakeWalk::Get().SimulateAndPrint(Args, Ar);
		}));
