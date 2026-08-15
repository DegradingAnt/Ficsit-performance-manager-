// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMStageTables.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMLeverRegistry.h"
#include "Core/FPMUserSettingMap.h"
#include "Fixes/Interop/FPMTexturePoolGuard.h"   // FPMTexturePool::QueryTotalVramMB, the one VRAM site

#include "Algo/Reverse.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDevice.h"

const TCHAR* LexToString(const EFPMStageTier Tier)
{
	switch (Tier)
	{
	case EFPMStageTier::None:       return TEXT("none");
	case EFPMStageTier::B1:         return TEXT("B1");
	case EFPMStageTier::B2:         return TEXT("B2");
	case EFPMStageTier::B3:         return TEXT("B3");
	case EFPMStageTier::B4:         return TEXT("B4");
	case EFPMStageTier::B5:         return TEXT("B5");
	case EFPMStageTier::B6:         return TEXT("B6");
	case EFPMStageTier::Resolution: return TEXT("R");
	case EFPMStageTier::K1:         return TEXT("K1");
	case EFPMStageTier::K2:         return TEXT("K2");
	case EFPMStageTier::K3:         return TEXT("K3");
	case EFPMStageTier::K4g:        return TEXT("K4g");
	case EFPMStageTier::K4f:        return TEXT("K4f");
	default:                        return TEXT("<unknown tier>");
	}
}

bool FPMIsBonusTier(const EFPMStageTier Tier)
{
	return Tier >= EFPMStageTier::B1 && Tier <= EFPMStageTier::B6;
}

bool FPMIsCutTier(const EFPMStageTier Tier)
{
	return Tier >= EFPMStageTier::K1 && Tier <= EFPMStageTier::K4f;
}

const TCHAR* LexToString(const EFPMGovernorMode Mode)
{
	switch (Mode)
	{
	case EFPMGovernorMode::Balanced:        return TEXT("Balanced");
	case EFPMGovernorMode::ResolutionFirst: return TEXT("A/resolution-first");
	case EFPMGovernorMode::GraphicsFirst:   return TEXT("B/graphics-first");
	case EFPMGovernorMode::LightingFirst:   return TEXT("C/lighting-first");
	default:                                return TEXT("<unknown mode>");
	}
}

namespace
{
	int32 StageIdx(const EFPMStageTier Tier) { return static_cast<int32>(Tier); }
	int32 StageIdx(const EFPMGovernorMode Mode) { return static_cast<int32>(Mode); }

	/** One declaration site for the group name the GI floor law (design section 3.4) is about. */
	const TCHAR* StageGIGroupName() { return TEXT("GlobalIlluminationQuality"); }

	/**
	 * The design's "@11.5GB" and "@15GB" annotations, turned into numbers a probe can use.
	 * The 15GB gate is 15000 and not 15500 on purpose: a 16GB card reports about 15209MB, and the
	 * design carries that receipt at :312 precisely because the tighter number silently disabled the
	 * tier on the exact hardware it was written for.
	 */
	constexpr int64 StageVram11500MB = 11500;
	constexpr int64 StageVram15000MB = 15000;

	/** Policies that compare against a baseline. Mirrors the registry's own list; repeated here only
	 *  to CHOOSE a BaselineSource before registration, and the registry still refuses if it is wrong. */
	bool StageNeedsBaseline(const EFPMLeverPolicy Policy)
	{
		return Policy == EFPMLeverPolicy::MaxOf || Policy == EFPMLeverPolicy::MinOf
			|| Policy == EFPMLeverPolicy::BaseScale || Policy == EFPMLeverPolicy::BaseDelta;
	}
}

const TCHAR* LexToString(const EFPMLeverPolarity Polarity)
{
	switch (Polarity)
	{
	case EFPMLeverPolarity::Unknown:        return TEXT("polarity-unknown");
	case EFPMLeverPolarity::HigherIsBetter: return TEXT("higher-is-better");
	case EFPMLeverPolarity::LowerIsBetter:  return TEXT("lower-is-better");
	default:                                return TEXT("<unknown polarity>");
	}
}

FString FPMFormatLeverValue(const float Value)
{
	if (FMath::IsNearlyEqual(Value, FMath::RoundToFloat(Value), 1.e-4f) && FMath::Abs(Value) < 1.e9f)
	{
		return FString::Printf(TEXT("%d"), static_cast<int32>(FMath::RoundToFloat(Value)));
	}
	FString Text = FString::Printf(TEXT("%.4f"), Value);
	while (Text.EndsWith(TEXT("0"))) { Text.LeftChopInline(1); }
	if (Text.EndsWith(TEXT("."))) { Text.LeftChopInline(1); }
	return Text;
}

float FPMProjectLeverValue(const FFPMStageLever& Lever, const float AssumedBaseline, FString& OutNote,
                           float* OutBeforeClamp)
{
	// Set the pre-clamp answer on EVERY exit path, including the two that return early. A caller that
	// judges the step's direction reads it, and an out-param left stale on an early return is the
	// shape that makes a direction verdict silently describe the previous lever.
	if (OutBeforeClamp) { *OutBeforeClamp = AssumedBaseline; }

	if (!Lever.GroupName.IsEmpty())
	{
		OutNote = TEXT("group lever: its target is a TIER, not a value. Use ResolveGroupTarget.");
		return AssumedBaseline;
	}

	const float Target = FCString::Atof(*Lever.TargetValue);
	float Result = AssumedBaseline;

	switch (Lever.Policy)
	{
	case EFPMLeverPolicy::Absolute:  Result = Target; break;
	case EFPMLeverPolicy::MaxOf:     Result = FMath::Max(AssumedBaseline, Target); break;
	case EFPMLeverPolicy::MinOf:     Result = FMath::Min(AssumedBaseline, Target); break;
	case EFPMLeverPolicy::BaseScale: Result = AssumedBaseline * Target; break;
	case EFPMLeverPolicy::BaseDelta: Result = AssumedBaseline + Target; break;
	default:
		OutNote = TEXT("no value arithmetic for this policy");
		return AssumedBaseline;
	}

	// The POLICY's own answer, before the range guard. This is the step.
	if (OutBeforeClamp) { *OutBeforeClamp = Result; }

	if (Lever.bHasClamp)
	{
		const float Clamped = FMath::Clamp(Result, Lever.ClampMin, Lever.ClampMax);
		if (!FMath::IsNearlyEqual(Clamped, Result))
		{
			OutNote = FString::Printf(TEXT("clamped to [%s, %s]"),
				*FPMFormatLeverValue(Lever.ClampMin), *FPMFormatLeverValue(Lever.ClampMax));
		}
		Result = Clamped;
	}
	return Result;
}

// ------------------------------------------------------------------------------------------------
// ★ THE INVARIANT CHECKS. Each one takes its subject as a PARAMETER so the self-test can run it over
// a deliberately violated copy and require a refusal. See the header for why that second run is the
// only thing that separates a working check from `return true`.
// ------------------------------------------------------------------------------------------------

namespace FPMStageInvariants
{
	/**
	 * The two K3 levers that raise a value inside a CUT tier, each with the reason it is allowed to.
	 *
	 * ⚠ THIS LIST IS DELIBERATELY TINY AND IS COUNTED BY ITS CALLER. An exemption list that can grow
	 * without anybody noticing turns the law it exempts from into a suggestion. Both entries are
	 * printed on every self-test run, with their reasons, whether or not the check passes.
	 */
	struct FStepExemption
	{
		EFPMStageTier Tier;
		const TCHAR*  CVarName;
		const TCHAR*  Reason;
	};

	static const FStepExemption GStepExemptions[] =
	{
		{ EFPMStageTier::K3, TEXT("r.Lumen.ScreenProbeGather.SpatialFilterNumPasses"),
		  TEXT("stated in the table itself: extra spatial filtering is the COMPENSATION half of the "
		       "same tier's downsample, so the rule is about the tier's net effect") },
		{ EFPMStageTier::K3, TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"),
		  TEXT("[AWAITING RULING: policy-gap] the design states the value and not the policy here, "
		       "and MaxOf is an inferred reading. The exemption stands only while the inference does") },
	};

	static const FStepExemption* FindExemption(const EFPMStageTier Tier, const FString& CVarName)
	{
		for (const FStepExemption& E : GStepExemptions)
		{
			if (E.Tier == Tier && CVarName.Equals(E.CVarName, ESearchCase::IgnoreCase))
			{
				return &E;
			}
		}
		return nullptr;
	}

	EFPMLeverPolarity DerivePolarity(const TArray<FFPMFlatLever>& Levers, const FString& CVarName)
	{
		bool bSawMax = false;
		bool bSawMin = false;
		for (const FFPMFlatLever& Flat : Levers)
		{
			if (!FPMIsBonusTier(Flat.Tier)) { continue; }
			if (!Flat.Lever.CVarName.Equals(CVarName, ESearchCase::IgnoreCase)) { continue; }
			if (Flat.Lever.Policy == EFPMLeverPolicy::MaxOf) { bSawMax = true; }
			if (Flat.Lever.Policy == EFPMLeverPolicy::MinOf) { bSawMin = true; }
		}
		// Both votes is a CONTRADICTION, not a tie to be broken. Reporting Unknown makes the caller
		// abstain rather than pick one, and the caller counts abstentions.
		if (bSawMax && !bSawMin) { return EFPMLeverPolarity::HigherIsBetter; }
		if (bSawMin && !bSawMax) { return EFPMLeverPolarity::LowerIsBetter; }
		return EFPMLeverPolarity::Unknown;
	}

	bool GIFloorNeverBreached(const TArray<FFPMFlatLever>& GroupLevers, FGroupResolver Resolver,
	                          const int32 FloorTier, FString& OutFailure, int32& OutCasesChecked)
	{
		OutCasesChecked = 0;
		// Every tier a live group could report, INCLUDING the two below the floor. A player whose own
		// setting sits at @1 is a real case, and the clamp must refuse a further step from there as
		// firmly as it refuses one from @2.
		for (const FFPMFlatLever& Flat : GroupLevers)
		{
			if (Flat.Lever.GroupName.IsEmpty()) { continue; }
			for (int32 LiveTier = 0; LiveTier <= 4; ++LiveTier)
			{
				FString Note;
				const int32 Landed = Resolver(Flat.Lever, LiveTier, Note);
				++OutCasesChecked;
				if (Landed == INDEX_NONE) { continue; }
				if (Landed < FloorTier)
				{
					OutFailure = FString::Printf(
						TEXT("%s's %s lever steps %+d and from @%d it LANDED on @%d, below the @%d "
						     "floor. @0 and @1 both carry %s=0, which is the 0.52.0 lighting "
						     "regression."),
						LexToString(Flat.Tier), *Flat.Lever.GroupName, Flat.Lever.GroupStep,
						LiveTier, Landed, FloorTier, FFPMStageTables::ForbiddenGICVarName());
					return false;
				}
			}
		}
		if (OutCasesChecked == 0)
		{
			OutFailure = TEXT("0 cases checked -- no group lever was handed to the floor check, so a "
			                  "pass here would be an artefact of an empty input rather than a fact "
			                  "about the tables");
			return false;
		}
		return true;
	}

	bool ForbiddenGICVarAbsent(const TSet<FString>& LadderCVars, const TCHAR* PositiveControl,
	                           FString& OutFailure)
	{
		// The positive control first. A pass on an empty or broken extractor is worth nothing, and
		// this project has already shipped one zero-filter that hid the exact thing it was asked
		// about.
		if (!LadderCVars.Contains(PositiveControl))
		{
			OutFailure = FString::Printf(
				TEXT("the ladder cvar set has %d entries and does NOT contain the positive control "
				     "'%s'. Every 'the forbidden cvar is absent' verdict from this set would be an "
				     "empty-extractor artefact."), LadderCVars.Num(), PositiveControl);
			return false;
		}
		for (const FString& Name : LadderCVars)
		{
			if (Name.Equals(FFPMStageTables::ForbiddenGICVarName(), ESearchCase::IgnoreCase))
			{
				OutFailure = FString::Printf(
					TEXT("'%s' is reachable from the ladder (%d cvar(s) checked). It is the GI kill "
					     "switch and no FPM path may name it."),
					FFPMStageTables::ForbiddenGICVarName(), LadderCVars.Num());
				return false;
			}
		}
		return true;
	}

	bool OrderNamesRealTiers(const TArray<EFPMStageTier>& Order, const TCHAR* Label, FString& OutFailure)
	{
		for (const EFPMStageTier Tier : Order)
		{
			// EMPTINESS IS NOT CHECKED HERE, ON PURPOSE. K4g derives to the empty set on this engine
			// build and is still a real, correctly named tier. A check that failed on that would be a
			// gate refusing correct input.
			if (Tier != EFPMStageTier::Resolution && !FPMIsBonusTier(Tier) && !FPMIsCutTier(Tier))
			{
				OutFailure = FString::Printf(
					TEXT("%s names '%s', which is neither a bonus tier, a cut tier, nor the "
					     "resolution step."), Label, LexToString(Tier));
				return false;
			}
		}
		return true;
	}

	bool StepDirectionsSane(const TArray<FFPMFlatLever>& Levers, FString& OutFailure,
	                        int32& OutChecked, int32& OutNoPolarity, int32& OutExempt,
	                        int32& OutInBandLandingCases)
	{
		OutChecked = 0;
		OutNoPolarity = 0;
		OutExempt = 0;
		OutInBandLandingCases = 0;
		OutFailure.Reset();

		// The sweep RUNS TO COMPLETION and this carries the verdict. The old code returned on the
		// first failure, which left OutExempt reading 0 while two exemptions existed and were printed
		// one line below it -- a count that contradicted the list beside it. OutFailure still holds the
		// FIRST failure; only the counters changed meaning, from "up to the first break" to "the whole
		// table".
		bool bAllHeld = true;

		// A SWEEP, not one baseline. A direction rule that holds at 1.0 and breaks at 0.25 is not a
		// rule. The sweep deliberately reaches OUTSIDE every lever's clamp band, which is why the two
		// laws below are judged separately -- see the header, and see what one merged verdict did to
		// K3's r.Lumen.ScreenProbeGather.DownsampleFactor.
		const float Baselines[] = { 0.0f, 0.25f, 1.0f, 4.0f, 100.0f, 4096.0f };

		for (const FFPMFlatLever& Flat : Levers)
		{
			const FFPMStageLever& Lever = Flat.Lever;
			if (!Lever.GroupName.IsEmpty()) { continue; }
			if (!FPMIsBonusTier(Flat.Tier) && !FPMIsCutTier(Flat.Tier)) { continue; }

			const EFPMLeverPolarity Polarity = DerivePolarity(Levers, Lever.CVarName);
			if (Polarity == EFPMLeverPolarity::Unknown)
			{
				++OutNoPolarity;
				continue;
			}
			if (const FStepExemption* Exemption = FindExemption(Flat.Tier, Lever.CVarName))
			{
				(void)Exemption;
				++OutExempt;
				continue;
			}

			++OutChecked;
			const bool bIsBonus = FPMIsBonusTier(Flat.Tier);

			// ONE verdict per lever-and-baseline, against whichever of the two laws it breaks. The
			// STEP is the policy's own arithmetic; the LANDING is what the clamp let through. They are
			// judged separately because a clamp pulling an out-of-band baseline back into the band is
			// the clamp obeying its contract, not the tier changing quality.
			auto Verdict = [&](const float Baseline, const float Value, const TCHAR* Which) -> bool
			{
				if (FMath::IsNearlyEqual(Value, Baseline, 1.e-6f)) { return true; }

				const bool bRose = Value > Baseline;
				const bool bImproved = (Polarity == EFPMLeverPolarity::HigherIsBetter) ? bRose : !bRose;
				if (bIsBonus == bImproved) { return true; }

				// Only the FIRST failure is reported, but the sweep runs on so the coverage counters
				// below describe the whole table rather than the prefix before the first break.
				if (!OutFailure.IsEmpty()) { return false; }

				// The format string is a LITERAL and the varying words are arguments. FString::Printf
				// static_asserts that its format is a TCHAR ARRAY, and a ternary between two literals
				// decays to a pointer, so "pick the sentence with ?:" does not compile here.
				OutFailure = FString::Printf(
					TEXT("%s %s lever '%s' (%s, %s) %s at the %s: baseline %s -> %s.%s"),
					bIsBonus ? TEXT("BONUS") : TEXT("CUT"),
					LexToString(Flat.Tier), *Lever.CVarName, LexToString(Lever.Policy),
					LexToString(Polarity),
					bIsBonus ? TEXT("WORSENS") : TEXT("IMPROVES"),
					Which, *FPMFormatLeverValue(Baseline), *FPMFormatLeverValue(Value),
					bIsBonus ? TEXT("")
					         : TEXT(" Only the two named K3 exemptions may do that, and this is not "
					                "one of them."));
				return false;
			};

			// THE SHARED SWEEP, PLUS THIS LEVER'S OWN BAND. The shared baselines deliberately sit
			// outside every clamp band, and for a narrow band they can miss it ENTIRELY: K3's
			// r.Lumen.ScreenProbeGather.DownsampleFactor is clamped to [8,32] and not one shared
			// baseline lands inside it. Law (b) would then have zero cases and be indistinguishable
			// from a constant true, which is the dead-instrument shape this file exists to avoid. So a
			// clamped lever is swept at its band's two ends and its middle as well.
			TArray<float, TInlineAllocator<16>> Sweep;
			Sweep.Append(Baselines, static_cast<int32>(UE_ARRAY_COUNT(Baselines)));
			if (Lever.bHasClamp)
			{
				Sweep.Add(Lever.ClampMin);
				Sweep.Add(Lever.ClampMax);
				Sweep.Add((Lever.ClampMin + Lever.ClampMax) * 0.5f);
			}

			for (const float Baseline : Sweep)
			{
				FString ProjNote;
				float BeforeClamp = Baseline;
				const float Landed = FPMProjectLeverValue(Lever, Baseline, ProjNote, &BeforeClamp);

				// (a) THE STEP. The law as written, and it has no range: policy arithmetic is judged
				// at every baseline in the sweep whether or not the clamp would have accepted it.
				if (!Verdict(Baseline, BeforeClamp, TEXT("STEP (pre-clamp)"))) { bAllHeld = false; }

				// (b) THE LANDING, only where the clamp's own contract applies. A baseline outside
				// [ClampMin, ClampMax] is an input the clamp exists to reject, so the direction it
				// lands in says nothing about the tier. Inside the band the contract does apply, and a
				// clamp that flips the landing there is a real defect and still fails here.
				const bool bBaselineInBand = !Lever.bHasClamp
					|| (Baseline >= Lever.ClampMin && Baseline <= Lever.ClampMax);
				if (bBaselineInBand)
				{
					if (Lever.bHasClamp) { ++OutInBandLandingCases; }
					if (!Verdict(Baseline, Landed, TEXT("LANDING (post-clamp, in-band baseline)")))
					{
						bAllHeld = false;
					}
				}
			}
		}

		if (OutChecked == 0)
		{
			OutFailure = TEXT("0 levers had a derivable polarity, so this check proved nothing about "
			                  "the tables");
			return false;
		}
		return bAllHeld;
	}

	bool SameNameSet(const TArray<FString>& A, const TArray<FString>& B, FString& OutDelta)
	{
		TArray<FString> SortedA = A;
		TArray<FString> SortedB = B;
		SortedA.Sort();
		SortedB.Sort();
		if (SortedA == SortedB)
		{
			OutDelta.Reset();
			return true;
		}

		FString Added;
		FString Removed;
		for (const FString& Name : SortedB)
		{
			if (!SortedA.Contains(Name)) { Added += FString::Printf(TEXT("+%s "), *Name); }
		}
		for (const FString& Name : SortedA)
		{
			if (!SortedB.Contains(Name)) { Removed += FString::Printf(TEXT("-%s "), *Name); }
		}
		OutDelta = FString::Printf(TEXT("%d before, %d after. %s%s"),
			SortedA.Num(), SortedB.Num(), *Removed, *Added);
		return false;
	}
}

const TCHAR* FFPMStageTables::ForbiddenGICVarName()
{
	return TEXT("r.Lumen.DiffuseIndirect.Allow");
}

EFPMGroupMemberExclusion FPMClassifyGroupMember(const FString& Member)
{
	// Section 3.4's one floor law. It reads ForbiddenGICVarName rather than repeating the string,
	// because that accessor's own comment claims to be "one declaration site" and DeriveK4gMembers
	// used to carry a second copy of the literal -- two copies of a law is how the law drifts.
	if (Member.Equals(FFPMStageTables::ForbiddenGICVarName(), ESearchCase::IgnoreCase))
	{
		return EFPMGroupMemberExclusion::ForbiddenGICVar;
	}
	if (FPMUserSettingMap::IsBacked(*Member))
	{
		return EFPMGroupMemberExclusion::UserSettingBacked;
	}
	return EFPMGroupMemberExclusion::None;
}

FFPMStageTables& FFPMStageTables::Get()
{
	static FFPMStageTables Instance;
	return Instance;
}

void FFPMStageTables::FlattenLevers(TArray<FFPMFlatLever>& Out) const
{
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		for (const FFPMStageLever& Lever : TierLevers[TierIndex])
		{
			FFPMFlatLever Flat;
			Flat.Tier = static_cast<EFPMStageTier>(TierIndex);
			Flat.Lever = Lever;
			Out.Add(MoveTemp(Flat));
		}
	}
}

void FFPMStageTables::CollectLadderCVars(TSet<FString>& Out, int32* OutGIKillSwitchExclusions) const
{
	if (OutGIKillSwitchExclusions) { *OutGIKillSwitchExclusions = 0; }

	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		const EFPMGovernorMode Mode = static_cast<EFPMGovernorMode>(M);
		// GIVE and TAKE both, even though one is the reverse of the other. The reversal is proven
		// elsewhere; a law check that assumed it would be leaning on another check's result.
		for (const EFPMStageTier Tier : GiveOrder(Mode)) { UnderlyingCVars(Tier, Out, OutGIKillSwitchExclusions); }
		for (const EFPMStageTier Tier : TakeOrder(Mode)) { UnderlyingCVars(Tier, Out, OutGIKillSwitchExclusions); }
	}
	// K4g's derived members are not registered levers, so UnderlyingCVars cannot see them. They are
	// still names a future arming of that tier would write, so the forbidden-cvar law covers them.
	for (const FString& Member : K4gMembers) { Out.Add(Member); }
}

void FFPMStageTables::GetAwaitingRulings(TArray<FString>& Out) const
{
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		for (const FFPMStageLever& Lever : TierLevers[TierIndex])
		{
			if (Lever.AwaitingRuling.IsEmpty()) { continue; }
			Out.Add(FString::Printf(TEXT("AWAITING RULING [%s] %s %s -- %s"),
				*Lever.AwaitingRuling, LexToString(static_cast<EFPMStageTier>(TierIndex)),
				*Lever.CVarName, *Lever.Note));
		}
	}
}

const TArray<FFPMStageLever>& FFPMStageTables::LeversIn(const EFPMStageTier Tier) const
{
	static const TArray<FFPMStageLever> Empty;
	const int32 I = StageIdx(Tier);
	return (I >= 0 && I < StageIdx(EFPMStageTier::Count)) ? TierLevers[I] : Empty;
}

const TArray<EFPMStageTier>& FFPMStageTables::GiveOrder(const EFPMGovernorMode Mode) const
{
	static const TArray<EFPMStageTier> Empty;
	const int32 I = StageIdx(Mode);
	return (I >= 0 && I < StageIdx(EFPMGovernorMode::Count)) ? GiveOrders[I] : Empty;
}

const TArray<EFPMStageTier>& FFPMStageTables::TakeOrder(const EFPMGovernorMode Mode) const
{
	static const TArray<EFPMStageTier> Empty;
	const int32 I = StageIdx(Mode);
	return (I >= 0 && I < StageIdx(EFPMGovernorMode::Count)) ? TakeOrders[I] : Empty;
}

// ------------------------------------------------------------------------------------------------
// The per-mode orders. Design :283-289, transcribed, then the take side DERIVED by reversal rather
// than typed a second time. "TAKE order = exact reverse (LIFO)" (section 3.1, :263) is a rule, and a
// rule implemented by construction cannot drift from a hand-typed copy of itself.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::BuildOrders()
{
	using T = EFPMStageTier;

	// A: "B6 to B1 demote, R: Max to AppliedMin (slew, concurrent), K1 K2 K3 K4g K4f".
	GiveOrders[StageIdx(EFPMGovernorMode::ResolutionFirst)] =
		{ T::B6, T::B5, T::B4, T::B3, T::B2, T::B1, T::Resolution, T::K1, T::K2, T::K3, T::K4g, T::K4f };

	// B: "R to AppliedMin FIRST (fast slew; cuts suppressed until pinned), B6 to B1, K1 to K4f".
	GiveOrders[StageIdx(EFPMGovernorMode::GraphicsFirst)] =
		{ T::Resolution, T::B6, T::B5, T::B4, T::B3, T::B2, T::B1, T::K1, T::K2, T::K3, T::K4g, T::K4f };

	// C: "R to AppliedMin, demote B6,B4,B5,B3,B2,B1, cuts K1,K2,K4f,K3,K4g".
	GiveOrders[StageIdx(EFPMGovernorMode::LightingFirst)] =
		{ T::Resolution, T::B6, T::B4, T::B5, T::B3, T::B2, T::B1, T::K1, T::K2, T::K4f, T::K3, T::K4g };

	// Balanced: section 3.5a (:441-447) is explicit. "Levers it may move: NONE of the stage tables",
	// and resolution is "the only GPU-SIDE lever Balanced steers". So its order is one entry long.
	// This is not a stub: a Balanced walk that offered a cut would be the pre-bench lock leaking.
	GiveOrders[StageIdx(EFPMGovernorMode::Balanced)] = { T::Resolution };

	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		TakeOrders[M] = GiveOrders[M];
		Algo::Reverse(TakeOrders[M]);
	}
}

// ------------------------------------------------------------------------------------------------
// Registration. Every lever goes into FFPMLeverRegistry FIRST; only a definition the registry
// ACCEPTED is kept here. A refusal (US_*-backed, sg.* named directly, missing baseline declaration)
// means the lever is not ours to move, and that refusal is the answer rather than a thing to route
// around.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::RegisterOne(const EFPMStageTier Tier, FFPMStageLever Lever,
                                  const EFPMLeverCurrency Currency,
                                  const TCHAR* EstimatedCost, const TCHAR* Provenance)
{
	const bool bGroup = !Lever.GroupName.IsEmpty();
	Lever.RegistryKey = FName(*FString::Printf(TEXT("Stage.%s.%s"),
		LexToString(Tier), bGroup ? *Lever.GroupName : *Lever.CVarName));

	FFPMLeverDefinition Def;
	Def.Name = Lever.RegistryKey;
	if (bGroup)
	{
		Def.Backing = EFPMLeverBacking::ScalabilityGroup;
		Def.GroupName = Lever.GroupName;
		Def.Policy = EFPMLeverPolicy::ScalabilityGroup;
	}
	else
	{
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { Lever.CVarName };
		Def.Policy = Lever.Policy;
	}
	Def.Currency = Currency;
	Def.EstimatedCost = EstimatedCost;
	Def.Side = EFPMFixSide::NeverOnDedicatedServer;
	Def.Provenance = Provenance;
	Def.TierHint = LexToString(Tier);

	// LAW 3. A baseline-comparing policy must declare where its baseline comes from, and the enum
	// offers no live-read option at all. ShippedTable is the preferred source, but no shipped
	// vanilla-defaults table exists for engine r.* cvars yet (FPMLeverTypes.h says so on
	// EFPMLeverBaselineSource::ShippedTable itself), so these declare CapturedOnce, the guarded path
	// where FFPMLeverRegistry::CaptureBaselineOnce refuses to read while FPM already holds the cvar.
	// That refusal IS the anti-ratchet guard: without it a second capture reads our own previous
	// write back as the player's baseline.
	Def.BaselineSource = StageNeedsBaseline(Def.Policy)
		? EFPMLeverBaselineSource::CapturedOnce
		: EFPMLeverBaselineSource::NotApplicable;

	// Section 3.12's capability probe. Where the design gates a lever on VRAM, the gate becomes a
	// probe rather than a comment: the lever reports ABSENT on a card that cannot hold it, which is
	// what FPM1 already did for dead rungs and what the design asks to keep ("stage lever ABSENT").
	if (Lever.VramGateMB > 0 && !bGroup)
	{
		const FString CVarCopy = Lever.CVarName;
		const int64 GateMB = Lever.VramGateMB;
		Def.CapabilityProbe = [CVarCopy, GateMB]() -> bool
		{
			if (!IConsoleManager::Get().FindConsoleVariable(*CVarCopy, false))
			{
				return false;
			}
			const int64 Vram = FPMTexturePool::QueryTotalVramMB();
			// Vram == 0 means the RHI has not answered yet. Treating "unknown" as "big enough" is how
			// an 8GB card gets handed a 15GB tier, so unknown fails the gate.
			return Vram > 0 && Vram >= GateMB;
		};
	}

	if (FFPMLeverRegistry::Get().RegisterWritable(MoveTemp(Def)) == nullptr)
	{
		RefusedByRegistry.Add(FString::Printf(TEXT("%s : %s"),
			LexToString(Tier), bGroup ? *Lever.GroupName : *Lever.CVarName));
		return;
	}

	TierLevers[StageIdx(Tier)].Add(MoveTemp(Lever));
}

// ------------------------------------------------------------------------------------------------
// THE TABLES. Design :304-338, transcribed lever by lever.
//
// Every cvar literal below was checked against the SHIPPED ENGINE SOURCE this session
// (C:/Program Files/Unreal Engine - CSS/Engine/Source, searched for the quoted literal). 42 of the
// 45 names resolved to a registration site. The three that did not:
//   grass.densityScale -- the design writes grass.DensityScale; the engine's own spelling is
//     lowercase-d at Runtime/Landscape/Private/LandscapeGrass.cpp:130. UE console lookup is
//     case-insensitive so either resolves, and the engine's spelling is what ships here.
//   pool.light.relevancyMultiplier / pool.light.shadowquality -- NOT engine cvars. They are CSS
//     main-DLL, data-driven through CFG/DefaultGame.ini and CFG/DefaultScalability.ini, confirmed
//     real with quoted help text by RESEARCH-A-CVAR-SYNTHESIS-2026-08-08.md:114 and :144. No static
//     source can confirm them, so they carry the runtime capability probe and nothing more.
//
// Policies marked "policy inferred" are ones the design's table states a VALUE for but no policy.
// MaxOf is the inferred choice on a bonus and MinOf on a cut, because the table's own header says
// MaxOf/MinOf "never worsen a user's own baseline" (:304) and that is the conservative reading. Each
// one says so on its own line rather than in a note at the top that nobody reads beside the lever.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::RegisterTables()
{
	using P = EFPMLeverPolicy;
	using T = EFPMStageTier;
	const EFPMLeverCurrency Gpu = EFPMLeverCurrency::GpuMs;
	const EFPMLeverCurrency GpuVram = EFPMLeverCurrency::GpuMs | EFPMLeverCurrency::VramMb;

	auto L = [](const TCHAR* CVarName, const TCHAR* Value, const P Policy, const TCHAR* Note = TEXT(""))
	{
		FFPMStageLever Lever;
		Lever.CVarName = CVarName;
		Lever.TargetValue = Value;
		Lever.Policy = Policy;
		Lever.Note = Note;
		return Lever;
	};
	auto Vram = [](FFPMStageLever Lever, const int64 GateMB)
	{
		Lever.VramGateMB = GateMB;
		return Lever;
	};
	auto Clamp = [](FFPMStageLever Lever, const float Min, const float Max)
	{
		Lever.bHasClamp = true;
		Lever.ClampMin = Min;
		Lever.ClampMax = Max;
		return Lever;
	};
	auto Group = [](const TCHAR* GroupName, const int32 Step, const TCHAR* Note)
	{
		FFPMStageLever Lever;
		Lever.GroupName = GroupName;
		Lever.GroupStep = Step;
		Lever.Policy = P::ScalabilityGroup;
		Lever.Note = Note;
		return Lever;
	};

	const TCHAR* InferMax = TEXT("policy inferred: the design states the value, not the policy; MaxOf "
	                             "is the never-worsen reading for a bonus");

	// ★ THE TWO RULING IDS THIS FILE OWNS. Ant owes a ruling on both, and until she gives one the
	// SHIPPED BEHAVIOUR STAYS EXACTLY AS IT IS: the marker labels the value, it does not change it.
	//   policy-gap -- the design's table states a VALUE for these levers and no POLICY. The inferred
	//                 reading is in the note beside each one.
	//   K4f-depth  -- the design states K4f's shape and its shallowness in words and names no cvars
	//                 and no values. The 0.75 scale is a [DEFAULT] with no mover.
	const TCHAR* RulingPolicyGap = TEXT("policy-gap");
	const TCHAR* RulingK4fDepth  = TEXT("K4f-depth");

	auto Ruling = [](FFPMStageLever Lever, const TCHAR* Id)
	{
		Lever.AwaitingRuling = Id;
		return Lever;
	};

	// ---- BONUS +1 (B1). Design :306. Lumen filtering plus reflection richness, about 0.5ms. -------
	{
		const TCHAR* C = TEXT("+1 tier, about 0.5ms shared across 11 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :306 (archive FPMQualityStages.cpp:205-236, byte-verified)");
		RegisterOne(T::B1, L(TEXT("r.Lumen.Reflections.TraceCompaction.WaveOps"), TEXT("1"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B1, L(TEXT("r.Lumen.RadianceCache.SortTraceTiles"), TEXT("1"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B1, Ruling(L(TEXT("r.Lumen.ScreenProbeGather.SpatialFilterNumPasses"), TEXT("4"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B1, Ruling(L(TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B1, Ruling(L(TEXT("r.Lumen.RadianceCache.SpatialFilterProbes"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B1, Ruling(L(TEXT("r.Lumen.Reflections.SampleSceneColorAtHit"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B1, Ruling(L(TEXT("r.Lumen.Reflections.RadianceCache"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B1, L(TEXT("r.Lumen.Reflections.BilateralFilter.NumSamples"), TEXT("6"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B1, L(TEXT("r.Lumen.Reflections.BilateralFilter.KernelRadius"), TEXT("10"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B1, L(TEXT("r.Nanite.Streaming.QualityScale.MaxPoolPercentage"), TEXT("92"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B1, L(TEXT("r.Nanite.Streaming.QualityScale.MinQuality"), TEXT("0.6"), P::MaxOf), Gpu, C, V);
	}

	// ---- BONUS +2 (B2). Design :307. Translucency plus radiosity polish, about 0.8ms. -------------
	{
		const TCHAR* C = TEXT("+2 tier, about 0.8ms shared across 6 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :307 (archive FPMQualityStages.cpp:237-310)");
		RegisterOne(T::B2, L(TEXT("r.Tonemapper.Sharpen"), TEXT("0.5"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B2, L(TEXT("r.TSR.ShadingRejection.Flickering"), TEXT("1"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B2, L(TEXT("grass.TickInterval"), TEXT("1"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B2, L(TEXT("r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated"), TEXT("32"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B2, L(TEXT("pool.light.relevancyMultiplier"), TEXT("10"), P::MaxOf,
			TEXT("CSS main-DLL cvar, not in engine source; probe-only confirmation")), Gpu, C, V);
		RegisterOne(T::B2, L(TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.NumSamples"), TEXT("7"), P::MaxOf), Gpu, C, V);
	}

	// ---- BONUS +3 (B3). Design :308. Trace reach plus surface cache, about 1.5ms. -----------------
	{
		const TCHAR* C = TEXT("+3 tier, about 1.5ms shared across 7 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :308 (archive FPMQualityStages.cpp:311-346)");
		RegisterOne(T::B3, L(TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction"), TEXT("1"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B3, L(TEXT("r.Lumen.TraceDistanceScale"), TEXT("2.0"), P::Absolute), Gpu, C, V);
		RegisterOne(T::B3, Ruling(L(TEXT("r.Lumen.TraceMeshSDFs"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B3, Ruling(L(TEXT("r.Lumen.TraceMeshSDFs.Allow"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::B3, Vram(L(TEXT("r.LumenScene.SurfaceCache.CardTexelDensityScale"), TEXT("800"), P::Absolute,
			TEXT("design gates this at 11.5GB VRAM")), StageVram11500MB), GpuVram, C, V);
		RegisterOne(T::B3, L(TEXT("r.LumenScene.GlobalSDF.Resolution"), TEXT("256"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B3, L(TEXT("r.VolumetricFog.GridPixelSize"), TEXT("6"), P::MinOf), Gpu, C, V);
	}

	// ---- BONUS +4 (B4). Design :309. Foliage plus Nanite sharpness. -------------------------------
	// The design names these "the only two live foliage levers" and records that CullDistanceScale is
	// inert in Shipping (:309, archive :458-474). The dead one is deliberately not carried.
	{
		const TCHAR* C = TEXT("+4 tier, cost not stated in the carried table");
		const TCHAR* V = TEXT("carried FPM1 table, design :309 (archive FPMQualityStages.cpp:458-474)");
		RegisterOne(T::B4, L(TEXT("grass.densityScale"), TEXT("2.0"), P::MaxOf,
			TEXT("engine spelling is lowercase-d, LandscapeGrass.cpp:130")), Gpu, C, V);
		RegisterOne(T::B4, L(TEXT("foliage.LODDistanceScale"), TEXT("2.0"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B4, L(TEXT("r.Nanite.MaxPixelsPerEdge"), TEXT("0.8"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B4, L(TEXT("r.Nanite.ViewMeshLODBias.Offset"), TEXT("-0.5"), P::MinOf), Gpu, C, V);
	}

	// ---- BONUS +5 (B5). Design :310. Cinematic GI: ONE lever, and it is the group. ----------------
	// The design is explicit that the direct sg.* write is US_*-backed and forbidden (:310, :541-558).
	// The registry enforces that structurally: this registers as ScalabilityGroup backing, and the
	// real writes are the alias table's MEMBER cvars.
	{
		RegisterOne(T::B5, Group(StageGIGroupName(), +1,
			TEXT("one CSS tier up from the player's own; the direct sg.* write is US_*-backed and forbidden")),
			Gpu,
			TEXT("+5 tier, 4.0ms, deliberately pessimistic"),
			TEXT("carried FPM1 table, design :310 (archive FPMQualityStages.cpp:476-493)"));
	}

	// ---- BONUS +6 (B6). Design :312. Beyond-Cinematic, about 3.0ms. -------------------------------
	{
		const TCHAR* C = TEXT("+6 tier, about 3.0ms shared across 13 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :312 (archive FPMQualityStages.cpp:565-615)");
		RegisterOne(T::B6, Vram(L(TEXT("r.Shadow.Virtual.MaxPhysicalPages"), TEXT("12288"), P::MaxOf,
			TEXT("design gates this at 11.5GB VRAM")), StageVram11500MB), GpuVram, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.Reflections.DownsampleFactor"), TEXT("1"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), TEXT("4"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"), TEXT("16"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget"), TEXT("512"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.TraceDistanceScale"), TEXT("4.0"), P::Absolute), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.TraceMeshSDFs.TraceDistance"), TEXT("250"), P::Absolute), Gpu, C, V);
		RegisterOne(T::B6, Vram(L(TEXT("r.LumenScene.SurfaceCache.CardTexelDensityScale"), TEXT("2000"), P::Absolute,
			TEXT("design gates this at 15GB VRAM, and the gate number is 15000 not 15500 because a 16GB "
			     "card reports about 15209MB")), StageVram15000MB), GpuVram, C, V);
		RegisterOne(T::B6, L(TEXT("r.VolumetricFog.GridSizeZ"), TEXT("192"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.VolumetricFog.HistoryMissSupersampleCount"), TEXT("16"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Nanite.MaxPixelsPerEdge"), TEXT("0.6"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Nanite.ViewMeshLODBias.Offset"), TEXT("-1.0"), P::MinOf), Gpu, C, V);
		RegisterOne(T::B6, L(TEXT("r.Lumen.ScreenProbeGather.SpatialFilterHalfKernelSize"), TEXT("2"), P::MaxOf), Gpu, C, V);
	}

	// ---- CUT -1 (K1). Design :322. Invisible trims: geometry plus post, NO lighting, about 0.4ms. --
	{
		const TCHAR* C = TEXT("K1 tier, about 0.4ms shared across 6 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :322 (archive FPMQualityStages.cpp:660-670)");
		RegisterOne(T::K1, L(TEXT("r.Nanite.MaxPixelsPerEdge"), TEXT("1.5"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::K1, L(TEXT("r.Nanite.ViewMeshLODBias.Offset"), TEXT("0.5"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::K1, L(TEXT("r.Tonemapper.Sharpen"), TEXT("0"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K1, L(TEXT("r.VolumetricFog.GridPixelSize"), TEXT("16"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::K1, L(TEXT("r.VolumetricFog.GridSizeZ"), TEXT("64"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K1, L(TEXT("r.VolumetricFog.HistoryMissSupersampleCount"), TEXT("4"), P::MinOf), Gpu, C, V);
	}

	// ---- CUT -2 (K2). Design :323. Reflections plus shadow budget, about 0.8ms. -------------------
	// r.Nanite.Streaming.StreamingPoolSize is DELIBERATELY ABSENT and must stay absent: raising the
	// pool past the hardware maximum is UE_LOG(..., Fatal, ...) at NaniteStreamingManager.cpp:1341-1343,
	// an engine crash. The design carries the corrected mechanism at :323.
	{
		const TCHAR* C = TEXT("K2 tier, about 0.8ms shared across 4 cvars");
		const TCHAR* V = TEXT("carried FPM1 table, design :323");
		RegisterOne(T::K2, L(TEXT("r.Lumen.Reflections.DownsampleFactor"), TEXT("2"), P::MaxOf), Gpu, C, V);
		RegisterOne(T::K2, L(TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.NumSamples"), TEXT("4"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K2, L(TEXT("r.Shadow.Virtual.MaxPhysicalPages"), TEXT("2048"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K2, L(TEXT("pool.light.relevancyMultiplier"), TEXT("2"), P::MinOf,
			TEXT("CSS main-DLL cvar, not in engine source; probe-only confirmation")), Gpu, C, V);
	}

	// ---- CUT -3 (K3). Design :330. GI plus shadows, about 1.4ms. GROUP FIRST, then hand levers. ---
	// The group step is FLOOR-CLAMPED AT @2 by FFPMStageTables::ResolveGroupTarget, never by
	// arithmetic at the call site. Design :330 closes review finding H1 with it: a bare -1 from a @2
	// baseline lands on @1, which carries r.Lumen.DiffuseIndirect.Allow=0, and that is the 0.52.0
	// regression the project has now refused three separate times.
	{
		const TCHAR* C = TEXT("K3 tier, about 1.4ms shared across the group step and 17 hand levers");
		const TCHAR* V = TEXT("carried FPM1 table, design :330 (archive FPMQualityStages.cpp:690-753)");
		RegisterOne(T::K3, Group(StageGIGroupName(), -1,
			TEXT("floor-clamped at @2; if the step would land below the floor it reports 'at GI floor, "
			     "skipped' and the walk proceeds to the next lever")), Gpu, C, V);
		RegisterOne(T::K3, Clamp(L(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), TEXT("2.0"), P::BaseScale), 8.0f, 32.0f), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.ScreenProbeGather.SpatialFilterNumPasses"), TEXT("4"), P::MaxOf,
			TEXT("raised, not lowered: extra spatial filtering compensates for the downsample above. "
			     "The 'a cut never IMPROVES a value' rule is about the tier's net effect, and this "
			     "lever is the compensation half of it")), Gpu, C, V);
		RegisterOne(T::K3, Ruling(L(TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"), TEXT("1"), P::MaxOf, InferMax), RulingPolicyGap), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"), TEXT("1.0"), P::BaseDelta), Gpu, C, V);
		RegisterOne(T::K3, Clamp(L(TEXT("r.Lumen.TraceDistanceScale"), TEXT("0.5"), P::BaseScale), 0.25f, 1.0f), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.TraceMeshSDFs"), TEXT("0"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.TraceMeshSDFs.TraceDistance"), TEXT("100"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.LumenScene.GlobalSDF.Resolution"), TEXT("128"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.LumenScene.SurfaceCache.CardTexelDensityScale"), TEXT("200"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"), TEXT("4"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget"), TEXT("128"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Lumen.ScreenProbeGather.SpatialFilterHalfKernelSize"), TEXT("1"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Shadow.Virtual.SMRT.RayCountDirectional"), TEXT("4"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Shadow.Virtual.SMRT.SamplesPerRayDirectional"), TEXT("2"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel"), TEXT("4"), P::MinOf), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving"), TEXT("1.0"), P::BaseDelta), Gpu, C, V);
		RegisterOne(T::K3, L(TEXT("pool.light.shadowquality"), TEXT("0"), P::MinOf,
			TEXT("CSS main-DLL cvar; documented range is 0..2, and 0 is in range. FPM1 wrote 4 here at "
			     "a different tier, which was out of range; that value is not carried")), Gpu, C, V);
	}

	// ---- CUT -4f (K4f). Design :336. Last resort, FOLIAGE, shallow. -------------------------------
	// The design states the SHAPE of this tier and its depth in words ("shallowly", and an early boot
	// test where aggressive cull "looks terrible") but names no cvars and no values for it. The two
	// live foliage levers are known from B4 (:309); the DEPTH is the part nobody has ruled.
	// [DEFAULT, needs a mover] BaseScale 0.75 of the captured baseline, chosen as the shallowest step
	// that is still measurable. It is registered so the tier is not silently empty in an order that
	// names it, and the report prints it as a DEFAULT awaiting a mover rather than as carried truth.
	{
		const TCHAR* C = TEXT("K4f tier, about 0.8ms");
		const TCHAR* V = TEXT("design :336 states the shape and the shallowness but no cvars and no "
		                      "values; the two cvars are the live foliage pair from :309 and the 0.75 "
		                      "depth is a DEFAULT awaiting a mover (design section 4.6)");
		RegisterOne(T::K4f, Ruling(Clamp(L(TEXT("grass.densityScale"), TEXT("0.75"), P::BaseScale,
			TEXT("[DEFAULT, needs a mover] depth not stated by the design")), 0.25f, 1.0f),
			RulingK4fDepth), Gpu, C, V);
		RegisterOne(T::K4f, Ruling(Clamp(L(TEXT("foliage.LODDistanceScale"), TEXT("0.75"), P::BaseScale,
			TEXT("[DEFAULT, needs a mover] depth not stated by the design")), 0.25f, 1.0f),
			RulingK4fDepth), Gpu, C, V);
	}

	// K4g is NOT registered here. Its lever list is DERIVED from the live BaseScalability.ini at
	// world load. See DeriveK4gMembers, which is also where the derivation's measured result is
	// recorded, because on this engine build it derives to the empty set.
}

// ------------------------------------------------------------------------------------------------
// ★ K4g's DERIVATION, AND THE MEASURED FACT THAT IT COMES OUT EMPTY.
//
// Design :334 specifies K4g's lever list as: diff(BaseScalability @2 -> @1), MINUS
// r.Lumen.DiffuseIndirect.Allow, MINUS anything US_*-backed.
//
// Computed against the shipped ini this session
// (C:/Program Files/Unreal Engine - CSS/Engine/Config/BaseScalability.ini:271-307):
//   [GlobalIlluminationQuality@1] carries FIVE keys. [GlobalIlluminationQuality@2] carries 27.
//   The ONLY key whose VALUE differs between the two tiers is r.Lumen.DiffuseIndirect.Allow (1 -> 0).
//   The other 22 Lumen keys exist at @2 and are ABSENT at @1, so stepping down never writes them.
//   Therefore diff(@2 -> @1) is exactly { r.Lumen.DiffuseIndirect.Allow }, and the law removes it.
//   K4g is the EMPTY SET on this engine build.
//
// That matters because K4g is named in every mode's order and the design calls it "the deepest
// lighting cut that exists". A tier in the walk that can never move anything is the dead-instrument
// shape, so it is reported as inert with this reason rather than silently doing nothing.
//
// The derivation is done HERE, at world load, from the LIVE ini rather than from a table baked at
// build time, for the reason IFPMFix::OnWorldLoad already gives: derive on the user's machine from
// what they actually have. If a future game or engine update populates @1 with real member values,
// this fills itself in.
//
// ⚠ IT DERIVES BUT DOES NOT REGISTER. Even a non-empty result cannot be armed without a per-cvar
// POLICY DIRECTION, and that direction is not derivable: for some members a cut means MinOf and for
// others (a downsample factor, a probe spacing) a cut means MaxOf. Inventing the direction is exactly
// the invention this build must not make, so a non-empty derivation is REPORTED and left unarmed with
// that reason. The set is empty today, so nothing is lost by it; the day it is not, the reason is
// printed instead of a wrong-direction write.
//
// Membership comes from the registry's alias table (its one declaration site). Only the VALUES are
// read here, and only for the member names the alias table already named, because "what does @1
// contain" and "what value does @1 give this key" are two different questions and the alias table
// answers the first by design.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::DeriveK4gMembers()
{
	K4gMembers.Reset();
	TierLevers[StageIdx(EFPMStageTier::K4g)].Reset();

	const FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
	const TArray<FString>* MembersAt1 = Registry.GetAliasMembers(StageGIGroupName(), 1);
	const TArray<FString>* MembersAt2 = Registry.GetAliasMembers(StageGIGroupName(), 2);

	if (!MembersAt1 || !MembersAt2)
	{
		K4gDerivationNote = TEXT("could not derive: the alias table has no @1 and/or @2 section for "
		                         "GlobalIlluminationQuality. This is a COVERAGE gap, not an empty "
		                         "result, and the two read differently on purpose.");
		return;
	}
	if (!GConfig)
	{
		K4gDerivationNote = TEXT("could not derive: GConfig is null, so the tier VALUES cannot be read");
		return;
	}

	const FString Section1 = FString::Printf(TEXT("%s@1"), StageGIGroupName());
	const FString Section2 = FString::Printf(TEXT("%s@2"), StageGIGroupName());

	int32 Considered = 0;
	int32 SameValue = 0;
	int32 ExcludedByLaw = 0;
	int32 ExcludedAsUserSetting = 0;

	for (const FString& Member : *MembersAt1)
	{
		++Considered;

		FString ValueAt1;
		FString ValueAt2;
		const bool bHas1 = GConfig->GetString(*Section1, *Member, ValueAt1, GScalabilityIni);
		const bool bHas2 = GConfig->GetString(*Section2, *Member, ValueAt2, GScalabilityIni);

		// A key @1 sets identically to @2 is not part of the delta: stepping down would write the
		// same number back. A key @2 does not carry at all IS part of the delta, because stepping
		// down would write it where nothing was written before.
		if (bHas1 && bHas2 && ValueAt1.Equals(ValueAt2, ESearchCase::IgnoreCase))
		{
			++SameValue;
			continue;
		}
		if (!bHas1)
		{
			continue;
		}

		// Section 3.4's floor law and Law 1, both from FPMClassifyGroupMember so this branch and
		// UnderlyingCVars cannot hold different ideas of what a group step may write. This code used
		// to carry its own copy of the rule; UnderlyingCVars carried none, and the kill switch stayed
		// reachable through the B5/K3 group levers for exactly as long as that asymmetry lasted.
		switch (FPMClassifyGroupMember(Member))
		{
		case EFPMGroupMemberExclusion::ForbiddenGICVar:   ++ExcludedByLaw;         continue;
		case EFPMGroupMemberExclusion::UserSettingBacked: ++ExcludedAsUserSetting; continue;
		default: break;
		}

		K4gMembers.Add(Member);
	}

	K4gDerivationNote = FString::Printf(
		TEXT("derived from the live BaseScalability.ini: %d member(s) at @1 considered, %d carried the "
		     "same value as @2 (no delta), %d excluded by the GI floor law, %d excluded as US_*-backed, "
		     "%d survived. Survivors are NOT armed: a cut needs a per-cvar policy direction that cannot "
		     "be derived from the ini, and inventing one is worse than an honest gap. "
		     "[AWAITING RULING: K4g-empty] the design calls K4g the deepest lighting cut that exists "
		     "and every mode order names it, so a tier that derives to nothing is Ant's call to make, "
		     "not this file's. The shipped behaviour is unchanged: K4g reports INERT."),
		Considered, SameValue, ExcludedByLaw, ExcludedAsUserSetting, K4gMembers.Num());
}

// ------------------------------------------------------------------------------------------------
// The order gate.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::UnderlyingCVars(const EFPMStageTier Tier, TSet<FString>& Out,
                                      int32* OutGIKillSwitchExclusions) const
{
	const FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
	for (const FFPMStageLever& Lever : LeversIn(Tier))
	{
		if (!Lever.CVarName.IsEmpty())
		{
			Out.Add(Lever.CVarName);
			continue;
		}
		// A group lever's real writes are its alias MEMBERS, so those are its underlying cvars. Only
		// the REACHABLE tiers count: section 3.4 puts the floor at @2 and the alias table stops at @3,
		// so @0 and @1 can never be applied and their members are not values this lever can touch.
		//
		// ★ AND THE GI KILL SWITCH IS NOT ONE OF THEM. BOTH reachable tiers carry it: BaseScalability.ini
		// sets r.Lumen.DiffuseIndirect.Allow=1 at GlobalIlluminationQuality@2 (:253) and again at @3
		// (:282), and 0 only at @0 (:239) and @1 (:246), which the floor law already puts out of reach.
		// So before this filter existed a B5 or K3 group step expanded to a member list that NAMED the
		// kill switch, twice per visit, which is what self-test (8) reported.
		// The law is categorical -- no FPM path may name that cvar, at any value -- so the exclusion
		// belongs at the expansion, where it removes the write, not at the audit, where it would only
		// remove the evidence.
		//
		// Only ForbiddenGICVar is filtered here, not every EFPMGroupMemberExclusion. A US_*-backed
		// member is refused at the write boundary by Law 1 anyway, and leaving it in this set can at
		// worst make CheckOrderInversions report a pair it need not have; that error is loud at the
		// next boot. Dropping it could hide a real shared write, and that error is silent.
		for (int32 GroupTier = GIGroupFloorTier(); GroupTier <= GroupCeilingTier(); ++GroupTier)
		{
			if (const TArray<FString>* Members = Registry.GetAliasMembers(Lever.GroupName, GroupTier))
			{
				for (const FString& Member : *Members)
				{
					if (FPMClassifyGroupMember(Member) == EFPMGroupMemberExclusion::ForbiddenGICVar)
					{
						if (OutGIKillSwitchExclusions) { ++(*OutGIKillSwitchExclusions); }
						continue;
					}
					Out.Add(Member);
				}
			}
		}
	}
}

namespace
{
	/**
	 * The pair-wise inversion test, factored out so the self-test can run it against a KNOWN-BAD
	 * order as well as the shipped ones. A check that is only ever run on data it passes is
	 * indistinguishable from a check that returns true.
	 */
	bool StageOrderPairsClear(const TArray<EFPMStageTier>& Canonical,
	                     const TArray<EFPMStageTier>& Candidate,
	                     const TFunctionRef<void(EFPMStageTier, TSet<FString>&)> Underlying,
	                     const TCHAR* Label,
	                     FString& OutFailure)
	{
		TMap<EFPMStageTier, int32> CanonPos;
		TMap<EFPMStageTier, int32> CandPos;
		for (int32 i = 0; i < Canonical.Num(); ++i) { CanonPos.Add(Canonical[i], i); }
		for (int32 i = 0; i < Candidate.Num(); ++i) { CandPos.Add(Candidate[i], i); }

		for (int32 a = 0; a < Candidate.Num(); ++a)
		{
			const int32* CanonA = CanonPos.Find(Candidate[a]);
			if (!CanonA) { continue; }
			for (int32 b = a + 1; b < Candidate.Num(); ++b)
			{
				const int32* CanonB = CanonPos.Find(Candidate[b]);
				if (!CanonB) { continue; }

				// The pair kept its relative order. Sharing a cvar with a neighbour is normal and
				// harmless, which is exactly why this check is about MOVEMENT and not adjacency.
				if (*CanonA < *CanonB) { continue; }

				TSet<FString> SetA;
				TSet<FString> SetB;
				Underlying(Candidate[a], SetA);
				Underlying(Candidate[b], SetB);
				const TSet<FString> Shared = SetA.Intersect(SetB);
				if (Shared.Num() > 0)
				{
					TArray<FString> SharedNames = Shared.Array();
					SharedNames.Sort();
					OutFailure = FString::Printf(
						TEXT("%s inverts %s before %s, and they share %d underlying cvar(s) including "
						     "'%s'. One order would apply them the other way round over the same value."),
						Label, LexToString(Candidate[a]), LexToString(Candidate[b]),
						Shared.Num(), *SharedNames[0]);
					return false;
				}
			}
		}
		return true;
	}
}

bool FFPMStageTables::CheckOrderInversions(FString& OutFailure) const
{
	const auto Underlying = [this](const EFPMStageTier Tier, TSet<FString>& Out)
	{
		UnderlyingCVars(Tier, Out);
	};

	// Mode A is the canonical order: the design writes it first and describes C as the reordering of
	// it. B differs only in where Resolution sits, and Resolution carries no levers here, so every
	// pair involving it is clear by construction rather than by luck.
	const TArray<EFPMStageTier>& Canonical = GiveOrder(EFPMGovernorMode::ResolutionFirst);

	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		const EFPMGovernorMode Mode = static_cast<EFPMGovernorMode>(M);
		if (Mode == EFPMGovernorMode::ResolutionFirst) { continue; }
		if (!StageOrderPairsClear(Canonical, GiveOrder(Mode), Underlying, LexToString(Mode), OutFailure))
		{
			return false;
		}
	}

	// The TAKE orders need no separate pass: each is the exact reverse of its own give order (built
	// that way, and check 1 of the self-test proves it), so a pair inverted between two take orders is
	// the same pair inverted between their give orders and has already been tested.
	return true;
}

int32 FFPMStageTables::ResolveGroupTarget(const FFPMStageLever& Lever, const int32 LiveTier,
                                          FString& OutNote) const
{
	if (Lever.GroupName.IsEmpty())
	{
		OutNote = TEXT("not a scalability-group lever");
		return INDEX_NONE;
	}

	// The floor is a property of the GROUP, not of the lever, and only GlobalIlluminationQuality has
	// one today (section 3.4). A different group added later gets its own answer here rather than
	// inheriting GI's, which would be a floor nobody chose.
	const int32 Floor = Lever.GroupName.Equals(StageGIGroupName(), ESearchCase::IgnoreCase)
		? GIGroupFloorTier() : 0;
	const int32 Target = LiveTier + Lever.GroupStep;

	if (Target < Floor)
	{
		OutNote = FString::Printf(
			TEXT("at GI floor, skipped: @%d %+d would land at @%d, below the @%d floor. @0 and @1 both "
			     "carry r.Lumen.DiffuseIndirect.Allow=0, which is the 0.52.0 lighting regression."),
			LiveTier, Lever.GroupStep, Target, Floor);
		return INDEX_NONE;
	}
	if (Target > GroupCeilingTier())
	{
		OutNote = FString::Printf(
			TEXT("at the group ceiling, skipped: @%d %+d would land at @%d, above @%d. The Cine tier is "
			     "deliberately outside the alias table."),
			LiveTier, Lever.GroupStep, Target, GroupCeilingTier());
		return INDEX_NONE;
	}

	OutNote = FString::Printf(TEXT("@%d -> @%d"), LiveTier, Target);
	return Target;
}

int32 FFPMStageTables::AvailableLeverCount(const EFPMStageTier Tier) const
{
	const FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
	int32 Count = 0;
	for (const FFPMStageLever& Lever : LeversIn(Tier))
	{
		const FFPMLeverDefinition* Def = Registry.Find(Lever.RegistryKey);
		if (Def && Def->Availability == EFPMLeverAvailability::Available)
		{
			++Count;
		}
	}
	return Count;
}

bool FFPMStageTables::IsTierInert(const EFPMStageTier Tier, FString& OutReason) const
{
	if (Tier == EFPMStageTier::Resolution)
	{
		OutReason = TEXT("resolution is owned by section 8 (native dyn-res, or the active upscaler's "
		                 "rung ladder). The stage tables carry no levers for it on purpose.");
		return false;
	}
	if (Tier == EFPMStageTier::K4g)
	{
		OutReason = K4gDerivationNote.IsEmpty()
			? FString(TEXT("K4g has not been derived yet (world load has not run)"))
			: K4gDerivationNote;
		return TierLevers[StageIdx(Tier)].Num() == 0;
	}
	if (!bProbed)
	{
		OutReason = TEXT("the capability probe pass has not run yet, so availability is unknown rather "
		                 "than zero");
		return false;
	}

	const int32 Available = AvailableLeverCount(Tier);
	if (Available == 0)
	{
		OutReason = FString::Printf(
			TEXT("0 of %d lever(s) available on this machine (absent cvar, refused registration, or an "
			     "unmet VRAM gate)"), LeversIn(Tier).Num());
		return true;
	}

	OutReason = FString::Printf(TEXT("%d of %d lever(s) available"), Available, LeversIn(Tier).Num());
	return false;
}

// ------------------------------------------------------------------------------------------------
// ★ THE SELF-TEST. Runs at WORLD LOAD, not at Arm, and that is load-bearing: check 4's known-bad
// order can only be proven bad once the alias table exists, and the alias table is built at world
// load. Running it at Arm would give a synthetic B5/B6 swap an EMPTY underlying-cvar set, the check
// would pass it, and the self-test would report a liveness proof it had not actually performed.
// ------------------------------------------------------------------------------------------------

bool FFPMStageTables::SelfTest()
{
	bool bOk = true;

	// (1) TAKE is the exact reverse of GIVE, element by element, for every mode. Section 3.1 states
	// it as a rule; this is the rule measured rather than trusted.
	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		const EFPMGovernorMode Mode = static_cast<EFPMGovernorMode>(M);
		const TArray<EFPMStageTier>& Give = GiveOrder(Mode);
		const TArray<EFPMStageTier>& Take = TakeOrder(Mode);
		bool bReversed = Give.Num() == Take.Num();
		for (int32 i = 0; bReversed && i < Give.Num(); ++i)
		{
			bReversed = Give[i] == Take[Give.Num() - 1 - i];
		}
		if (!bReversed)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (1) FAILED: %s take order is not the exact reverse "
				     "of its give order."), LexToString(Mode));
			bOk = false;
		}
	}

	// (2) INVARIANT: every tier an order names is a real tier, and an EMPTY tier is legitimate. A typo
	// in an order table would otherwise reach the walk as a step that matches nothing and is skipped
	// in silence. Run BOTH directions: the shipped orders must clear it, and an order carrying a
	// non-tier must be refused, or this check could be returning a constant.
	{
		for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
		{
			const EFPMGovernorMode Mode = static_cast<EFPMGovernorMode>(M);
			FString Failure;
			if (!FPMStageInvariants::OrderNamesRealTiers(GiveOrder(Mode), LexToString(Mode), Failure))
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] stage tables self-test (2a) FAILED: %s"), *Failure);
				bOk = false;
			}
			if (!FPMStageInvariants::OrderNamesRealTiers(TakeOrder(Mode), LexToString(Mode), Failure))
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] stage tables self-test (2a) FAILED on the take side: %s"), *Failure);
				bOk = false;
			}
		}

		TArray<EFPMStageTier> Bad = GiveOrder(EFPMGovernorMode::ResolutionFirst);
		Bad.Add(EFPMStageTier::None);
		FString BadFailure;
		if (FPMStageInvariants::OrderNamesRealTiers(Bad, TEXT("synthetic order with a non-tier"), BadFailure))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (2b) FAILED: an order carrying EFPMStageTier::None "
				     "was CLEARED. The tier-name check is not discriminating."));
			bOk = false;
		}

		// The EMPTY-TIER half, proven rather than asserted in a comment. K4g is named by every mode's
		// order and carries no levers on this engine build, and the check above must still pass.
		const bool bK4gNamed = GiveOrder(EFPMGovernorMode::ResolutionFirst).Contains(EFPMStageTier::K4g);
		if (!bK4gNamed)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (2c) FAILED: K4g is not named by mode A's order, so "
				     "the 'an empty tier is legitimate' half of this invariant was never exercised."));
			bOk = false;
		}
	}

	// (3) POSITIVE CONTROL ON THE UNDERLYING-CVAR EXTRACTOR ITSELF, before anything is concluded from
	// a zero. B5 is a group lever, so its underlying set comes entirely from the alias table; if that
	// set is empty, every later "they share nothing" verdict is an artefact of an empty extractor
	// rather than a fact about the tables.
	{
		TSet<FString> B5Under;
		UnderlyingCVars(EFPMStageTier::B5, B5Under);
		const bool bHasKnownMember = B5Under.Contains(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"));
		if (B5Under.Num() == 0 || !bHasKnownMember)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (3) FAILED: B5's underlying-cvar set has %d entries "
				     "and %s r.Lumen.ScreenProbeGather.DownsampleFactor, which BaseScalability.ini "
				     "carries at GlobalIlluminationQuality@2. Every 'no shared cvars' verdict below "
				     "would be an empty-extractor artefact."),
				B5Under.Num(), bHasKnownMember ? TEXT("contains") : TEXT("does NOT contain"));
			bOk = false;
		}
	}

	// (4) The order gate, BOTH directions. The shipped orders must PASS, and a deliberately inverted
	// order over a pair that really does share underlying cvars must FAIL.
	{
		FString Failure;
		if (!CheckOrderInversions(Failure))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (4a) FAILED: the SHIPPED orders did not clear the "
				     "inversion gate. %s"), *Failure);
			bOk = false;
		}

		TArray<EFPMStageTier> Synthetic = GiveOrder(EFPMGovernorMode::ResolutionFirst);
		const int32 PosB5 = Synthetic.IndexOfByKey(EFPMStageTier::B5);
		const int32 PosB6 = Synthetic.IndexOfByKey(EFPMStageTier::B6);
		if (PosB5 == INDEX_NONE || PosB6 == INDEX_NONE)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (4b) FAILED: the canonical order does not contain "
				     "both B5 and B6, so the known-bad case cannot be built."));
			bOk = false;
		}
		else
		{
			Synthetic.Swap(PosB5, PosB6);
			const auto Underlying = [this](const EFPMStageTier Tier, TSet<FString>& Out)
			{
				UnderlyingCVars(Tier, Out);
			};
			FString SyntheticFailure;
			const bool bSyntheticCleared = StageOrderPairsClear(
				GiveOrder(EFPMGovernorMode::ResolutionFirst), Synthetic, Underlying,
				TEXT("synthetic B5/B6 swap"), SyntheticFailure);
			if (bSyntheticCleared)
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] stage tables self-test (4b) FAILED: an order that swaps B5 and B6 was "
					     "CLEARED, but B5's group members and B6's hand levers both move "
					     "r.Lumen.ScreenProbeGather.DownsampleFactor. The gate is not discriminating."));
				bOk = false;
			}
		}
	}

	// (5) The GI floor clamp, BOTH directions, on the two real group levers. A clamp that only ever
	// refuses stops the ladder as surely as one that never refuses lets the regression back in.
	{
		const TArray<FFPMStageLever>& K3Levers = LeversIn(EFPMStageTier::K3);
		const TArray<FFPMStageLever>& B5Levers = LeversIn(EFPMStageTier::B5);
		const FFPMStageLever* CutGroup = K3Levers.FindByPredicate(
			[](const FFPMStageLever& L) { return !L.GroupName.IsEmpty(); });
		const FFPMStageLever* BonusGroup = B5Levers.FindByPredicate(
			[](const FFPMStageLever& L) { return !L.GroupName.IsEmpty(); });

		if (!CutGroup || !BonusGroup)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (5) FAILED: K3 and/or B5 has no group lever, so the "
				     "floor clamp cannot be exercised. K3 %s, B5 %s."),
				CutGroup ? TEXT("has one") : TEXT("has none"),
				BonusGroup ? TEXT("has one") : TEXT("has none"));
			bOk = false;
		}
		else
		{
			FString Note;
			const bool bRefusesAtFloor  = ResolveGroupTarget(*CutGroup, 2, Note) == INDEX_NONE;
			const bool bAllowsFromAbove = ResolveGroupTarget(*CutGroup, 3, Note) == 2;
			const bool bAllowsPromote   = ResolveGroupTarget(*BonusGroup, 2, Note) == 3;
			const bool bRefusesCeiling  = ResolveGroupTarget(*BonusGroup, 3, Note) == INDEX_NONE;
			if (!bRefusesAtFloor || !bAllowsFromAbove || !bAllowsPromote || !bRefusesCeiling)
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] stage tables self-test (5) FAILED: clamp verdicts were cut-at-floor "
					     "refused=%s, cut-from-@3 allowed=%s, promote-from-@2 allowed=%s, "
					     "promote-at-ceiling refused=%s. All four must hold."),
					bRefusesAtFloor ? TEXT("yes") : TEXT("NO"),
					bAllowsFromAbove ? TEXT("yes") : TEXT("NO"),
					bAllowsPromote ? TEXT("yes") : TEXT("NO"),
					bRefusesCeiling ? TEXT("yes") : TEXT("NO"));
				bOk = false;
			}
		}
	}

	// (6) Every lever kept here really is in the registry. The two copies exist on purpose (see the
	// header), so the join between them is a thing that can rot, and this is the only place it is
	// checked.
	{
		const FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
		int32 Orphans = 0;
		for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
		{
			for (const FFPMStageLever& Lever : TierLevers[TierIndex])
			{
				if (Registry.Find(Lever.RegistryKey) == nullptr)
				{
					++Orphans;
					UE_LOG(LogFicsitsPerformanceManager, Error,
						TEXT("[FPM] stage tables self-test (6): '%s' is in the tables but not in the "
						     "registry."), *Lever.RegistryKey.ToString());
				}
			}
		}
		if (Orphans > 0)
		{
			bOk = false;
		}
	}

	TArray<FFPMFlatLever> Flat;
	FlattenLevers(Flat);

	// (7) INVARIANT: THE GI GROUP NEVER MOVES BELOW @2 ON ANY PATH, and the proof that the check can
	// SAY NO. The same checker runs twice: once with the shipped resolver (must pass) and once with an
	// UNCLAMPED resolver that does the bare arithmetic (must fail). Without the second run this check
	// could not tell the clamp apart from an absent clamp.
	{
		TArray<FFPMFlatLever> GroupLevers;
		for (const FFPMFlatLever& Entry : Flat)
		{
			if (!Entry.Lever.GroupName.IsEmpty()) { GroupLevers.Add(Entry); }
		}

		const auto ShippedResolver = [this](const FFPMStageLever& Lever, const int32 LiveTier,
		                                    FString& Note)
		{
			return ResolveGroupTarget(Lever, LiveTier, Note);
		};
		// The known-bad path: what the arithmetic does with no floor at all. This is the code the
		// clamp replaced, and it must be caught.
		const auto UnclampedResolver = [](const FFPMStageLever& Lever, const int32 LiveTier,
		                                  FString& Note)
		{
			Note = TEXT("synthetic UNCLAMPED resolver -- deliberate violation");
			return LiveTier + Lever.GroupStep;
		};

		FString Failure;
		int32 Cases = 0;
		const bool bShippedHolds = FPMStageInvariants::GIFloorNeverBreached(
			GroupLevers, ShippedResolver, GIGroupFloorTier(), Failure, Cases);

		FString BadFailure;
		int32 BadCases = 0;
		const bool bUnclampedHolds = FPMStageInvariants::GIFloorNeverBreached(
			GroupLevers, UnclampedResolver, GIGroupFloorTier(), BadFailure, BadCases);

		if (!bShippedHolds || bUnclampedHolds)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (7) FAILED: shipped resolver held=%s across %d "
				     "case(s) (%s); UNCLAMPED resolver held=%s across %d case(s) (expected a refusal, "
				     "got '%s'). Both halves must be true or the floor clamp is unproven."),
				bShippedHolds ? TEXT("yes") : TEXT("NO"), Cases, *Failure,
				bUnclampedHolds ? TEXT("YES") : TEXT("no"), BadCases, *BadFailure);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] stage tables self-test (7) passed: the GI floor held across %d resolver "
				     "case(s), and the deliberately unclamped resolver was REFUSED with: %s"),
				Cases, *BadFailure);
		}
	}

	// (8) INVARIANT: r.Lumen.DiffuseIndirect.Allow NEVER APPEARS ON ANY LADDER, and the proof that the
	// check can say no. The positive control inside the checker guards against an empty extractor; the
	// injected copy proves the checker refuses the real thing.
	{
		TSet<FString> Ladder;
		int32 KillSwitchExclusions = 0;
		CollectLadderCVars(Ladder, &KillSwitchExclusions);

		FString Failure;
		const bool bClean = FPMStageInvariants::ForbiddenGICVarAbsent(
			Ladder, TEXT("r.Nanite.MaxPixelsPerEdge"), Failure);

		TSet<FString> Violated = Ladder;
		Violated.Add(ForbiddenGICVarName());
		FString BadFailure;
		const bool bViolatedCleared = FPMStageInvariants::ForbiddenGICVarAbsent(
			Violated, TEXT("r.Nanite.MaxPixelsPerEdge"), BadFailure);

		if (!bClean || bViolatedCleared)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (8) FAILED: the shipped ladder (%d cvar(s)) was "
				     "clean=%s (%s), and a copy with '%s' injected was cleared=%s (expected a "
				     "refusal, got '%s')."),
				Ladder.Num(), bClean ? TEXT("yes") : TEXT("NO"), *Failure,
				ForbiddenGICVarName(), bViolatedCleared ? TEXT("YES") : TEXT("no"), *BadFailure);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] stage tables self-test (8) passed: %d ladder cvar(s) checked, '%s' absent, "
				     "and the injected copy was REFUSED with: %s"),
				Ladder.Num(), ForbiddenGICVarName(), *BadFailure);

			// The filter's own coverage, printed whether or not it is comfortable. A clean ladder and a
			// dead group expansion read identically without this number, so zero is a WARNING rather
			// than a quiet line.
			if (KillSwitchExclusions > 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] stage tables self-test (8) coverage: the group expansion met '%s' and "
					     "dropped it %d time(s). That is the GI floor law doing work, not an empty set "
					     "reporting clean."),
					ForbiddenGICVarName(), KillSwitchExclusions);
			}
			else
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] stage tables self-test (8) coverage: the group expansion NEVER met '%s'. "
					     "Zero is not proof of a clean ladder -- it means no reachable "
					     "GlobalIlluminationQuality tier carried the kill switch at all. Either "
					     "BaseScalability.ini changed, or the alias table is empty and the group "
					     "expansion is dead."),
					ForbiddenGICVarName());
			}
		}
	}

	// (9) INVARIANT: no bonus step worsens a lever and no cut step improves one, across a baseline
	// sweep. The known-bad copy flips one cut lever's policy, which must be caught.
	{
		FString Failure;
		int32 Checked = 0;
		int32 NoPolarity = 0;
		int32 Exempt = 0;
		int32 InBandCases = 0;
		const bool bDirectionsHold = FPMStageInvariants::StepDirectionsSane(
			Flat, Failure, Checked, NoPolarity, Exempt, InBandCases);

		// Build the violation from a lever that really has a derived polarity, so the mutation is a
		// genuine law break and not an untestable no-op.
		TArray<FFPMFlatLever> Violated = Flat;
		bool bMutated = false;
		for (FFPMFlatLever& Entry : Violated)
		{
			if (Entry.Tier != EFPMStageTier::K1) { continue; }
			if (!Entry.Lever.CVarName.Equals(TEXT("r.Nanite.MaxPixelsPerEdge"))) { continue; }
			// K1 raises this cvar because lower is better for it. MinOf would LOWER it, which is a cut
			// tier improving a value.
			Entry.Lever.Policy = EFPMLeverPolicy::MinOf;
			Entry.Lever.TargetValue = TEXT("0.1");
			bMutated = true;
			break;
		}

		FString BadFailure;
		int32 BadChecked = 0;
		int32 BadNoPolarity = 0;
		int32 BadExempt = 0;
		int32 BadInBandCases = 0;
		const bool bViolatedHeld = FPMStageInvariants::StepDirectionsSane(
			Violated, BadFailure, BadChecked, BadNoPolarity, BadExempt, BadInBandCases);

		if (!bDirectionsHold || !bMutated || bViolatedHeld)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (9) FAILED: shipped directions held=%s (%s); the "
				     "known-bad copy was built=%s and held=%s (expected a refusal, got '%s'). "
				     "Coverage: %d lever(s) checked, %d abstained for want of a derivable polarity, "
				     "%d exempt by name, %d in-band landing case(s) for the clamp law."),
				bDirectionsHold ? TEXT("yes") : TEXT("NO"), *Failure,
				bMutated ? TEXT("yes") : TEXT("NO"),
				bViolatedHeld ? TEXT("YES") : TEXT("no"), *BadFailure,
				Checked, NoPolarity, Exempt, InBandCases);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] stage tables self-test (9) passed: %d lever(s) checked across the shared "
				     "baseline sweep plus each clamped lever's own band, %d abstained (no derivable "
				     "polarity -- an ABSTENTION, not a pass), %d exempt by name. The STEP law and the "
				     "in-band LANDING law were both judged, the latter over %d case(s). The known-bad "
				     "copy was REFUSED with: %s"),
				Checked, NoPolarity, Exempt, InBandCases, *BadFailure);
		}

		if (InBandCases == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] stage tables self-test (9) coverage: the in-band LANDING law judged ZERO "
				     "cases, so it proved nothing this run. It reads exactly like a pass. Either no "
				     "lever declares a clamp any more, or the band sweep stopped reaching the band."));
		}

		for (const FPMStageInvariants::FStepExemption& Exemption : FPMStageInvariants::GStepExemptions)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] stage tables self-test (9) EXEMPT: %s %s -- %s"),
				LexToString(Exemption.Tier), Exemption.CVarName, Exemption.Reason);
		}
	}

	// (10) THE AWAITING-RULING MARKER MUST NOT BE DEAD. Four rulings are owed and this file carries
	// three of them; a marker list that reached zero would look exactly like a list of settled
	// questions, so a zero is a failure here rather than good news.
	{
		TArray<FString> Rulings;
		GetAwaitingRulings(Rulings);
		if (Rulings.Num() == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] stage tables self-test (10) FAILED: 0 levers carry an AWAITING RULING "
				     "marker. Either every ruling has landed and this check must be retired, or the "
				     "marker has gone dead and every report since is quietly missing it."));
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] stage tables self-test (10) passed: %d lever(s) carry an AWAITING RULING "
				     "marker, and K4g's derivation note carries a fourth."), Rulings.Num());
		}
	}

	return bOk;
}

// ------------------------------------------------------------------------------------------------
// IFPMFix
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::Arm()
{
	BuildOrders();
	RegisterTables();

	int32 Total = 0;
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		Total += TierLevers[TierIndex].Num();
	}

	// FPM.Diag.Steering gates the arm and world-load lines at level 1. The console reports below are
	// deliberately NOT gated: an operator asking what the ladder holds must get an answer with
	// diagnostics off, the same reasoning HostTier and Detect already carry.
	if (!FPMDiag::IsOn(FPMDiag::EChannel::Steering))
	{
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] stage tables armed: %d lever(s) accepted by the registry, %d refused. K4g is "
		     "derived at world load, and the self-test runs there too (it needs the alias table). "
		     "NOTHING IS APPLIED FROM HERE -- these tables say WHAT a tier moves, never WHEN."),
		Total, RefusedByRegistry.Num());

	for (const FString& Refusal : RefusedByRegistry)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] stage tables: registry refused %s -- that refusal is the answer, and the tier "
			     "is smaller than the design's table by one lever."), *Refusal);
	}
}

void FFPMStageTables::OnWorldLoad(UWorld* World)
{
	DeriveK4gMembers();
	bProbed = true;
	bSelfTestPassed = SelfTest();

	int32 InertTiers = 0;
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		const EFPMStageTier Tier = static_cast<EFPMStageTier>(TierIndex);
		if (!FPMIsBonusTier(Tier) && !FPMIsCutTier(Tier)) { continue; }
		FString Reason;
		if (IsTierInert(Tier, Reason))
		{
			++InertTiers;
			UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] stage tier %s is INERT: %s"), LexToString(Tier), *Reason);
		}
	}

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] stage tables: self-test %s, %d of the 11 stage tiers are inert on this machine. "
		     "K4g derivation: %s"),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"), InertTiers, *K4gDerivationNote);
}

void FFPMStageTables::Disarm()
{
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		TierLevers[TierIndex].Reset();
	}
	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		GiveOrders[M].Reset();
		TakeOrders[M].Reset();
	}
	RefusedByRegistry.Reset();
	K4gMembers.Reset();
	K4gDerivationNote.Reset();
	bSelfTestPassed = false;
	bProbed = false;
}

// ------------------------------------------------------------------------------------------------
// Report.
// ------------------------------------------------------------------------------------------------

void FFPMStageTables::ReportNow(FOutputDevice& Ar) const
{
	FPMScopedConsoleEcho Echo(&Ar);

	if (!bSelfTestPassed)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] stage tables: the self-test has not passed (it runs at world load; see the log "
			     "for which check). REFUSING to print tables whose order gate and floor clamp are "
			     "unproven."));
		return;
	}

	int32 Total = 0;
	int32 Available = 0;
	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		const EFPMStageTier Tier = static_cast<EFPMStageTier>(TierIndex);
		Total += TierLevers[TierIndex].Num();
		Available += AvailableLeverCount(Tier);
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] stage tables -- %d lever(s) registered across 11 tiers, %d available on this "
		     "machine, %d refused by the registry. These tables are DATA: nothing here applies a "
		     "value, and the walk that decides when to move a tier is FPMGiveTake."),
		Total, Available, RefusedByRegistry.Num());

	for (int32 TierIndex = 0; TierIndex < StageIdx(EFPMStageTier::Count); ++TierIndex)
	{
		const EFPMStageTier Tier = static_cast<EFPMStageTier>(TierIndex);
		if (!FPMIsBonusTier(Tier) && !FPMIsCutTier(Tier)) { continue; }

		FString Reason;
		const bool bInert = IsTierInert(Tier, Reason);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-4s %-6s %s"), LexToString(Tier),
			bInert ? TEXT("INERT") : TEXT("live"), *Reason);

		for (const FFPMStageLever& Lever : LeversIn(Tier))
		{
			const FFPMLeverDefinition* Def = FFPMLeverRegistry::Get().Find(Lever.RegistryKey);
			const FString Marker = Lever.AwaitingRuling.IsEmpty()
				? FString()
				: FString::Printf(TEXT("[AWAITING RULING: %s] "), *Lever.AwaitingRuling);
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]        %-9s %-58s %-8s %-16s %s%s"),
				Def ? LexToString(Def->Availability) : TEXT("NOT-IN-REG"),
				Lever.GroupName.IsEmpty() ? *Lever.CVarName : *Lever.GroupName,
				Lever.GroupName.IsEmpty() ? *Lever.TargetValue : TEXT("(group)"),
				LexToString(Lever.Policy),
				*Marker, *Lever.Note);
		}
	}

	for (int32 M = 0; M < StageIdx(EFPMGovernorMode::Count); ++M)
	{
		const EFPMGovernorMode Mode = static_cast<EFPMGovernorMode>(M);
		FString Line;
		for (const EFPMStageTier Tier : GiveOrder(Mode))
		{
			Line += FString::Printf(TEXT("%s "), LexToString(Tier));
		}
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   GIVE order %-20s %s"), LexToString(Mode), *Line);
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   K4g: %s"), *K4gDerivationNote);
	for (const FString& Member : K4gMembers)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]        derived but NOT armed (no per-cvar policy direction): %s"), *Member);
	}
	for (const FString& Refusal : RefusedByRegistry)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   REFUSED BY THE REGISTRY: %s"), *Refusal);
	}

	// ★ THE OWED RULINGS, PRINTED LAST SO THEY ARE THE PART A READER LEAVES WITH. Each line names a
	// question that is Ant's to answer. The shipped behaviour beside each one is unchanged and stays
	// unchanged until she answers.
	TArray<FString> Rulings;
	GetAwaitingRulings(Rulings);
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d lever(s) carry an AWAITING RULING marker. A zero here would be a dead marker, "
		     "not a settled question, and the self-test fails on it."), Rulings.Num());
	for (const FString& Line : Rulings)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]        %s"), *Line);
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMStageReportCmd(
	TEXT("FPM.Stage.Report"),
	TEXT("Stage tables: the B and K tier content, its probe coverage on this machine, every inert "
	     "tier with its reason, and each mode's give order. Reads only."),
	// ⚠ THE GATE IS CONSTRUCTED HERE AND NOT INSIDE ReportNow, and the placement is the design.
	// ReportNow builds its own FPMScopedConsoleEcho, and the gate must come BEFORE the echo so a
	// refusal costs one line instead of a report. Keeping it in the lambda also leaves every
	// non-console caller of ReportNow ungated, which is what the automatic summaries need.
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMReportGate Gate(Ar, TEXT("FPM.Stage.Report"));
		if (Gate.IsRefused())
		{
			return;
		}

		FFPMStageTables::Get().ReportNow(Ar);
	}));
