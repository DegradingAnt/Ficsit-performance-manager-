// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMStageApply.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMLeverRegistry.h"
#include "Core/FPMMasterSwitch.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

const TCHAR* LexToString(const EFPMApplyOutcome Outcome)
{
	switch (Outcome)
	{
	case EFPMApplyOutcome::Written:               return TEXT("WRITTEN");
	case EFPMApplyOutcome::Released:              return TEXT("released");
	case EFPMApplyOutcome::SkippedNotRegistered:  return TEXT("skipped: not in the registry");
	case EFPMApplyOutcome::SkippedUnavailable:    return TEXT("skipped: unavailable here");
	case EFPMApplyOutcome::SkippedNoBaseline:     return TEXT("skipped: no safe baseline");
	case EFPMApplyOutcome::SkippedGroupStepRefused:   return TEXT("skipped: group step refused");
	case EFPMApplyOutcome::SkippedExcludedMember: return TEXT("skipped: excluded member");
	case EFPMApplyOutcome::WriterRefused:         return TEXT("REFUSED by the writer");
	default:                                      return TEXT("<unknown outcome>");
	}
}

namespace
{
	/**
	 * ★ THE APPLY PASS'S OWN SCRATCH CVAR, AND WHY IT IS NOT FPMCVarWriter's.
	 *
	 * The self-test needs a baseline that is not zero: a BaseScale lever applied to a baseline of 0
	 * lands on 0 no matter how many times it is applied, so the anti-ratchet check would pass
	 * against a live-read implementation too and would prove nothing. FPMCVarWriter's probe is an
	 * int at 0 and is shared with the residue sentinel, so this file declares its own float probe
	 * with a deliberately non-round default instead of nudging somebody else's.
	 *
	 * ⚠ IT IS OURS, so it is zero-residue by construction: registered by this module, gone when the
	 * module unloads, never written to any ini. Declaring our own console variable is not the thing
	 * Law 1 bans; writing a vanilla US_*-backed one is.
	 */
	const TCHAR* ApplyProbeName()
	{
		return TEXT("FPM.Stage.Apply.Probe");
	}

	constexpr float GApplyProbeDefault = 4.0f;

	TAutoConsoleVariable<float> GApplyProbeCVar(
		ApplyProbeName(),
		GApplyProbeDefault,
		TEXT("FPM internal. The apply pass's own scratch variable, used only by its arm-time proof "
		     "that a write writes and a release releases. Never read by any feature."),
		ECVF_Default);

	/** The self-test's lever keys. Prefixed so the registry counts them as fixtures, never as
	 *  shipped levers -- FPMLeverSelfTestPrefix() is what derives that flag. */
	FName ApplyFixtureScaleKey() { return FName(TEXT("__SelfTest.Apply.Scale")); }
	FName ApplyFixtureAbsentKey() { return FName(TEXT("__SelfTest.Apply.Absent")); }

	/** A name no console variable can carry. The known-negative for the availability gate, and a
	 *  DIFFERENT string from the registry's and the server levers' own known-absent names so no
	 *  single literal reaches four sites. */
	const TCHAR* ApplyKnownAbsentName()
	{
		return TEXT("fpm.stage.apply.this.console.variable.must.never.exist");
	}

	/** Read a console variable as text, or an honest marker. Used for the REPORT only. */
	FString ApplyReadCVarText(const FString& CVarName)
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*CVarName, false);
		return Var ? Var->GetString() : FString(TEXT("<no such cvar>"));
	}
}

FFPMStageApply& FFPMStageApply::Get()
{
	static FFPMStageApply Instance;
	return Instance;
}

FName FFPMStageApply::OwnerFor(const EFPMStageTier Tier)
{
	// Derived from the tier's own name, so a new tier cannot be given a hand-typed owner that drifts
	// from it, and so a hold can always be traced back to the tier identity that took it.
	return FName(*FString::Printf(TEXT("FPM.Stage.%s"), LexToString(Tier)));
}

EFPMStageTier FFPMStageApply::ParseTier(const FString& Text)
{
	for (int32 I = 1; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		const EFPMStageTier Tier = static_cast<EFPMStageTier>(I);
		if (Text.Equals(LexToString(Tier), ESearchCase::IgnoreCase))
		{
			return Tier;
		}
	}
	return EFPMStageTier::None;
}

bool FFPMStageApply::IsTierHeld(const EFPMStageTier Tier) const
{
	const int32 Index = static_cast<int32>(Tier);
	return Index > 0 && Index < static_cast<int32>(EFPMStageTier::Count) && bTierHeld[Index];
}

int32 FFPMStageApply::ReadLiveGroupTier(const FString& GroupName)
{
	// sg.<Group> is the group's own console variable. FPM can never WRITE it (FPMCVarWriter clause 2
	// refuses every sg.* write unconditionally), which is exactly why READING it is safe under Law 3:
	// the value can never be our own prior write coming back as a baseline.
	const FString CVarName = FString::Printf(TEXT("sg.%s"), *GroupName);
	const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*CVarName, false);
	return Var ? Var->GetInt() : INDEX_NONE;
}

// ------------------------------------------------------------------------------------------------
// One lever
// ------------------------------------------------------------------------------------------------

bool FFPMStageApply::ApplyCVarLever(const FFPMStageLever& Lever, const FName Owner,
                                    const TCHAR* Reason, FFPMApplyLine& OutLine)
{
	OutLine.CVarName = Lever.CVarName;
	OutLine.ObservedBefore = ApplyReadCVarText(Lever.CVarName);

	FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
	const FFPMLeverDefinition* Def = Registry.Find(Lever.RegistryKey);
	if (!Def)
	{
		// Not an absence to shrug at. A missing entry means RegisterWritable REFUSED this lever at
		// registration (Law 1 or clause 2) and the tier is genuinely smaller than the design's table.
		OutLine.Outcome = EFPMApplyOutcome::SkippedNotRegistered;
		OutLine.Why = FString::Printf(
			TEXT("'%s' is not in the lever registry -- it was refused at registration, so this tier "
			     "is one lever smaller than the design's table"), *Lever.RegistryKey.ToString());
		return false;
	}

	if (Def->Availability != EFPMLeverAvailability::Available)
	{
		// ⚠ Unknown is NOT Available. Before the probe pass has run we do not know whether the cvar
		// or the VRAM tier exists here, and treating unknown as fine is how a 15GB tier reaches an
		// 8GB card. The probe pass runs at world load; an apply before that is refused on purpose.
		OutLine.Outcome = EFPMApplyOutcome::SkippedUnavailable;
		OutLine.Why = FString::Printf(TEXT("capability probe says %s"),
			LexToString(Def->Availability));
		return false;
	}

	if (Lever.Policy == EFPMLeverPolicy::ScalabilityGroup)
	{
		// Structurally unreachable from the shipped tables (RegisterOne gives a group lever a
		// GroupName, and EngageTier routes on that), so this is a guard against a future table entry
		// rather than a live branch. It matters because the alternative is silent: FPMProjectLeverValue
		// has no arithmetic for this policy and would hand back the baseline, which the code below
		// would then write as if it were a target.
		OutLine.Outcome = EFPMApplyOutcome::SkippedNotRegistered;
		OutLine.Why = TEXT("a ScalabilityGroup policy reached the cvar path -- a group lever's target "
		                   "is a TIER, and it must carry a GroupName so EngageTier routes it to the "
		                   "group expansion instead");
		return false;
	}

	FString TargetText;
	FString Note;

	if (FPMPolicyNeedsBaseline(Lever.Policy))
	{
		// ★ LAW 3. The baseline is captured ONCE, before FPM's first hold of this cvar, and never
		// re-read. CaptureBaselineOnce refuses outright if the writer already holds it, which is what
		// makes a second engage project from the player's value rather than from our own last write.
		if (!Registry.CaptureBaselineOnce(Lever.RegistryKey))
		{
			OutLine.Outcome = EFPMApplyOutcome::SkippedNoBaseline;
			OutLine.Why = TEXT("CaptureBaselineOnce refused (it logs which of its four reasons). "
			                   "Refusing to write is correct: the alternative is inventing a baseline "
			                   "or re-reading a value we may have written ourselves.");
			return false;
		}

		// Re-find: the capture mutated the stored definition.
		Def = Registry.Find(Lever.RegistryKey);
		if (!Def || !Def->bBaselineCaptured || !Def->CapturedBaselineValue.IsNumeric())
		{
			OutLine.Outcome = EFPMApplyOutcome::SkippedNoBaseline;
			OutLine.Why = FString::Printf(
				TEXT("captured baseline '%s' is not a number, so the policy arithmetic has nothing to "
				     "work from"),
				Def ? *Def->CapturedBaselineValue : TEXT("<lever vanished>"));
			return false;
		}

		const float Baseline = FCString::Atof(*Def->CapturedBaselineValue);
		const float Landed = FPMProjectLeverValue(Lever, Baseline, Note);
		TargetText = FPMFormatLeverValue(Landed);
		if (Note.IsEmpty()) { Note = Lever.Note; }
		Note = FString::Printf(TEXT("%s policy from captured baseline %s%s%s"),
			LexToString(Lever.Policy), *FPMFormatLeverValue(Baseline),
			Note.IsEmpty() ? TEXT("") : TEXT(" -- "), *Note);
	}
	else
	{
		// Absolute: the design's table states the value outright, and no baseline is involved.
		const float Landed = FPMProjectLeverValue(Lever, 0.0f, Note);
		TargetText = FPMFormatLeverValue(Landed);
		if (Note.IsEmpty()) { Note = Lever.Note; }
		Note = FString::Printf(TEXT("Absolute%s%s"),
			Note.IsEmpty() ? TEXT("") : TEXT(" -- "), *Note);
	}

	OutLine.Target = TargetText;
	OutLine.Why = Note;

	if (!FPMCVarWriter::Get().Hold(Owner, *Lever.CVarName, *TargetText, Reason, EFPMLease::Module))
	{
		// The writer logs its own reason in full. Counted here so a tier that wrote nothing can never
		// be reported as a tier that had nothing to write.
		OutLine.Outcome = EFPMApplyOutcome::WriterRefused;
		OutLine.Why += TEXT(" [the writer refused; its own log line says which clause]");
		return false;
	}

	OutLine.Outcome = EFPMApplyOutcome::Written;
	return true;
}

void FFPMStageApply::ApplyGroupLever(const FFPMStageLever& Lever, const FName Owner,
                                     const TCHAR* Reason, FFPMApplyResult& InOut)
{
	const FFPMStageTables& Tables = FFPMStageTables::Get();
	FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();

	const int32 LiveTier = ReadLiveGroupTier(Lever.GroupName);
	if (LiveTier == INDEX_NONE)
	{
		FFPMApplyLine Line;
		Line.FromGroup = Lever.GroupName;
		Line.CVarName = FString::Printf(TEXT("sg.%s"), *Lever.GroupName);
		Line.Outcome = EFPMApplyOutcome::SkippedUnavailable;
		Line.Why = TEXT("the group's own console variable is not registered in this build, so there "
		                "is no live tier to step from");
		InOut.Lines.Add(MoveTemp(Line));
		++InOut.Skipped;
		return;
	}

	FString GroupNote;
	const int32 TargetTier = Tables.ResolveGroupTarget(Lever, LiveTier, GroupNote);
	if (TargetTier == INDEX_NONE)
	{
		// The GI floor clamp refused the step. The design's own wording: report it and let the walk
		// proceed to the next lever rather than stalling.
		FFPMApplyLine Line;
		Line.FromGroup = Lever.GroupName;
		Line.CVarName = FString::Printf(TEXT("sg.%s"), *Lever.GroupName);
		Line.ObservedBefore = FString::Printf(TEXT("@%d"), LiveTier);
		Line.Outcome = EFPMApplyOutcome::SkippedGroupStepRefused;
		Line.Why = GroupNote;
		InOut.Lines.Add(MoveTemp(Line));
		++InOut.Skipped;
		return;
	}

	const FFPMLeverDefinition* Def = Registry.Find(Lever.RegistryKey);
	if (!Def || Def->Availability != EFPMLeverAvailability::Available)
	{
		FFPMApplyLine Line;
		Line.FromGroup = Lever.GroupName;
		Line.CVarName = FString::Printf(TEXT("sg.%s"), *Lever.GroupName);
		Line.Outcome = Def ? EFPMApplyOutcome::SkippedUnavailable
		                   : EFPMApplyOutcome::SkippedNotRegistered;
		Line.Why = Def
			? FString::Printf(TEXT("capability probe says %s"), LexToString(Def->Availability))
			: FString(TEXT("the group lever is not in the registry -- refused at registration"));
		InOut.Lines.Add(MoveTemp(Line));
		++InOut.Skipped;
		return;
	}

	const TArray<FString>* Members = Registry.GetAliasMembers(Lever.GroupName, TargetTier);
	if (!Members || Members->Num() == 0)
	{
		FFPMApplyLine Line;
		Line.FromGroup = Lever.GroupName;
		Line.CVarName = FString::Printf(TEXT("sg.%s@%d"), *Lever.GroupName, TargetTier);
		Line.Outcome = EFPMApplyOutcome::SkippedUnavailable;
		Line.Why = TEXT("the alias table has no members for that tier -- BaseScalability.ini does not "
		                "carry the section, so a group step here would write nothing at all");
		InOut.Lines.Add(MoveTemp(Line));
		++InOut.Skipped;
		return;
	}

	for (const FString& Member : *Members)
	{
		++InOut.GroupMembersExamined;

		// ★ ONE CLASSIFIER, the same one FFPMStageTables::UnderlyingCVars and DeriveK4gMembers call.
		// The GI kill switch and the US_*-backed set are unreachable from every group expansion
		// because they are unreachable from THIS function, not because three copies of a rule agree.
		const EFPMGroupMemberExclusion Exclusion = FPMClassifyGroupMember(Member);
		if (Exclusion != EFPMGroupMemberExclusion::None)
		{
			if (Exclusion == EFPMGroupMemberExclusion::ForbiddenGICVar) { ++InOut.ExcludedGIKillSwitch; }
			else { ++InOut.ExcludedUserSetting; }

			FFPMApplyLine Line;
			Line.FromGroup = Lever.GroupName;
			Line.CVarName = Member;
			Line.Outcome = EFPMApplyOutcome::SkippedExcludedMember;
			Line.Why = (Exclusion == EFPMGroupMemberExclusion::ForbiddenGICVar)
				? TEXT("section 3.4's floor law: the GI kill switch is unreachable from every FPM path")
				: TEXT("Law 1: US_*-backed, so a hold would survive uninstall as the player's setting");
			InOut.Lines.Add(MoveTemp(Line));
			++InOut.Skipped;
			continue;
		}

		FString MemberValue;
		if (!Registry.GetAliasMemberValue(Lever.GroupName, TargetTier, Member, MemberValue))
		{
			FFPMApplyLine Line;
			Line.FromGroup = Lever.GroupName;
			Line.CVarName = Member;
			Line.Outcome = EFPMApplyOutcome::SkippedUnavailable;
			Line.Why = TEXT("the alias table names this member but carries no value for it");
			InOut.Lines.Add(MoveTemp(Line));
			++InOut.Skipped;
			continue;
		}

		FFPMApplyLine Line;
		Line.FromGroup = Lever.GroupName;
		Line.CVarName = Member;
		Line.ObservedBefore = ApplyReadCVarText(Member);
		Line.Target = MemberValue;
		Line.Why = FString::Printf(
			TEXT("group step @%d -> @%d, member value from the live BaseScalability.ini. %s"),
			LiveTier, TargetTier, *GroupNote);

		if (FPMCVarWriter::Get().Hold(Owner, *Member, *MemberValue, Reason, EFPMLease::Module))
		{
			Line.Outcome = EFPMApplyOutcome::Written;
			++InOut.Written;
		}
		else
		{
			Line.Outcome = EFPMApplyOutcome::WriterRefused;
			++InOut.Skipped;
		}
		InOut.Lines.Add(MoveTemp(Line));
	}
}

// ------------------------------------------------------------------------------------------------
// One tier
// ------------------------------------------------------------------------------------------------

FFPMApplyResult FFPMStageApply::EngageTier(const EFPMStageTier Tier, const EFPMSteerAction Action)
{
	FFPMApplyResult Result;
	Result.Tier = Tier;
	Result.Action = Action;
	Result.bExecuted = true;

	const FName Owner = OwnerFor(Tier);
	const FString ReasonText = FString::Printf(
		TEXT("governor: %s tier %s"), LexToString(Action), LexToString(Tier));

	// ONE APPLY PASS PER DECISION (section 9.1). Every lever of the tier is written in the table's
	// own order inside this single call, because each settings change hitches and N separate passes
	// would hitch N times.
	for (const FFPMStageLever& Lever : FFPMStageTables::Get().LeversIn(Tier))
	{
		if (!Lever.GroupName.IsEmpty())
		{
			ApplyGroupLever(Lever, Owner, *ReasonText, Result);
			continue;
		}

		FFPMApplyLine Line;
		if (ApplyCVarLever(Lever, Owner, *ReasonText, Line)) { ++Result.Written; }
		else                                                 { ++Result.Skipped; }
		Result.Lines.Add(MoveTemp(Line));
	}

	// ★ ONE LEVER WRITTEN MAKES THE TIER HELD, and that is the right answer for a PARTIAL apply
	// rather than a compromise. A tier whose levers are half absent on this machine is a smaller
	// tier, not a failed one -- section 3.12 already says a lever that cannot exist here reports
	// ABSENT and the ladder carries on. Release is by OWNER, so it drops exactly what was taken,
	// however much that was. What must never happen is the mirror: zero written and the walk told
	// the tier is engaged, which would make the next decision skip a rung that never moved.
	if (Result.Written > 0)
	{
		bTierHeld[static_cast<int32>(Tier)] = true;
		SessionWrites += Result.Written;
	}
	return Result;
}

FFPMApplyResult FFPMStageApply::ReleaseTier(const EFPMStageTier Tier, const EFPMSteerAction Action)
{
	FFPMApplyResult Result;
	Result.Tier = Tier;
	Result.Action = Action;
	Result.bExecuted = true;

	// ★ NOTHING IS RESTORED, SO NOTHING CAN BE RESTORED WRONGLY. Release is the engine's own
	// Unset(0x07, "FPM"): our entry leaves the priority history and whatever was underneath reappears.
	const int32 Dropped = FPMCVarWriter::Get().ReleaseOwner(OwnerFor(Tier));
	Result.Released = Dropped;
	SessionReleases += Dropped;

	FFPMApplyLine Line;
	Line.CVarName = FString::Printf(TEXT("(every hold owned by %s)"), *OwnerFor(Tier).ToString());
	Line.Outcome = EFPMApplyOutcome::Released;
	Line.Why = Dropped > 0
		? FString::Printf(TEXT("%d hold(s) dropped through the engine's tagged history"), Dropped)
		: FString(TEXT("this tier held nothing -- said out loud, because 'released it' and 'never "
		               "held it' are different facts and only one means a revert happened"));
	Result.Lines.Add(MoveTemp(Line));

	bTierHeld[static_cast<int32>(Tier)] = false;
	return Result;
}

// ------------------------------------------------------------------------------------------------
// The entry point
// ------------------------------------------------------------------------------------------------

FFPMApplyResult FFPMStageApply::Execute(const FFPMSteerDecision& Decision)
{
	FFPMApplyResult Result;
	Result.Tier = Decision.Tier;
	Result.Action = Decision.Action;

	if (!FPMMasterSwitch::IsEnabled())
	{
		Result.Refusal = TEXT("FPM.Enabled is 0. The master switch's OFF means RELEASED, so the apply "
		                      "pass refuses to take a new hold while the mod is off.");
		++SessionRefusals;
		return Result;
	}

	switch (Decision.Action)
	{
	case EFPMSteerAction::None:
		Result.Refusal = FString::Printf(
			TEXT("the decision carried no action (block: %s). Nothing to apply."),
			LexToString(Decision.Block));
		return Result;

	case EFPMSteerAction::ResolutionDown:
	case EFPMSteerAction::ResolutionUp:
		// ⚠ REFUSED, LOUDLY, RATHER THAN REPORTED AS DONE. Section 8 owns the resolution lever and
		// its closure rule is a Slice-3 investigation. A silent success here would tell the walk the
		// floor had been reached when nothing moved, which is precisely how the 0.55.0 ladder came to
		// believe things about a state it never produced.
		Result.Refusal = TEXT("resolution is owned by section 8 (native dyn-res, or the active "
		                      "upscaler's rung ladder) and its executor is not built. This apply pass "
		                      "carries no resolution lever, so it reports NOT EXECUTED instead of a "
		                      "success that did not happen.");
		++SessionRefusals;
		return Result;

	case EFPMSteerAction::EngageCut:
	case EFPMSteerAction::PromoteBonus:
	case EFPMSteerAction::ReleaseCut:
	case EFPMSteerAction::DemoteBonus:
		break;   // the four this file executes; which way is decided below

	default:
		Result.Refusal = TEXT("unrecognised action");
		++SessionRefusals;
		return Result;
	}

	const int32 TierIndex = static_cast<int32>(Decision.Tier);
	if (TierIndex <= 0 || TierIndex >= static_cast<int32>(EFPMStageTier::Count))
	{
		Result.Refusal = TEXT("the decision names no real tier");
		++SessionRefusals;
		return Result;
	}

	const bool bEngage = Decision.Action == EFPMSteerAction::EngageCut
	                  || Decision.Action == EFPMSteerAction::PromoteBonus;

	FFPMApplyResult Applied = bEngage
		? EngageTier(Decision.Tier, Decision.Action)
		: ReleaseTier(Decision.Tier, Decision.Action);

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] apply %s %s: %d written, %d released, %d skipped "
		     "(group members examined %d, GI-kill-switch exclusions %d, US_* exclusions %d)"),
		LexToString(Applied.Action), LexToString(Applied.Tier), Applied.Written, Applied.Released,
		Applied.Skipped, Applied.GroupMembersExamined, Applied.ExcludedGIKillSwitch,
		Applied.ExcludedUserSetting);

	return Applied;
}

int32 FFPMStageApply::ReleaseEverything(const TCHAR* Reason)
{
	int32 Total = 0;
	for (int32 I = 1; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		Total += FPMCVarWriter::Get().ReleaseOwner(OwnerFor(static_cast<EFPMStageTier>(I)));
		bTierHeld[I] = false;
	}
	SessionReleases += Total;

	UE_CLOG(Total > 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] apply pass: released every ladder hold (%d) -- %s"), Total, Reason);
	return Total;
}

// ------------------------------------------------------------------------------------------------
// The liveness proof. Every check has a known-positive AND a known-negative.
// ------------------------------------------------------------------------------------------------

bool FFPMStageApply::SelfTest()
{
	FFPMLeverRegistry& Registry = FFPMLeverRegistry::Get();
	const FName Owner(TEXT("FPMStageApply.SelfTest"));

	TArray<FString> HeldBefore;
	FPMCVarWriter::Get().GetHeldCVars(HeldBefore);

	// ⚠ THE SESSION COUNTERS ARE SAVED AND PUT BACK. Check (5) drives Execute on purpose, which
	// increments SessionRefusals, and without this the report would open every session claiming a
	// refusal the operator never caused. A counter that counts the test as well as the work is a
	// counter nobody can read.
	const int32 SavedWrites = SessionWrites;
	const int32 SavedReleases = SessionReleases;
	const int32 SavedRefusals = SessionRefusals;

	bool bOk = true;
	auto Fail = [&bOk](const FString& Line)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error, TEXT("[FPM] apply self-test FAILED: %s"), *Line);
		bOk = false;
	};

	// ---- The fixtures. Registered at Arm so the registry's own world-load probe pass classified
	// ---- them alongside every other lever; a fixture registered here would still read Unknown.
	const FFPMLeverDefinition* ScaleDef = Registry.Find(ApplyFixtureScaleKey());
	const FFPMLeverDefinition* AbsentDef = Registry.Find(ApplyFixtureAbsentKey());
	if (!ScaleDef || !AbsentDef)
	{
		Fail(TEXT("the self-test fixtures are not in the registry, so nothing below could be proved. "
		          "They register at Arm(); if that failed, the registry logged why."));
		return false;
	}
	if (ScaleDef->Availability != EFPMLeverAvailability::Available)
	{
		Fail(FString::Printf(
			TEXT("the scale fixture probed %s, but its cvar is one this file registers itself. Either "
			     "the probe pass has not run or the cvar failed to register."),
			LexToString(ScaleDef->Availability)));
		return false;
	}

	// A lever definition matching the fixture. Built here rather than taken from the stage tables,
	// because the tables must never carry a self-test entry.
	FFPMStageLever Scale;
	Scale.RegistryKey = ApplyFixtureScaleKey();
	Scale.CVarName = ApplyProbeName();
	Scale.Policy = EFPMLeverPolicy::BaseScale;
	Scale.TargetValue = TEXT("2.0");
	Scale.Note = TEXT("self-test fixture");

	FFPMStageLever Absent;
	Absent.RegistryKey = ApplyFixtureAbsentKey();
	Absent.CVarName = ApplyKnownAbsentName();
	Absent.Policy = EFPMLeverPolicy::Absolute;
	Absent.TargetValue = TEXT("1");
	Absent.Note = TEXT("self-test fixture, known-absent cvar");

	// ---- (1) KNOWN-POSITIVE: a write really writes. ------------------------------------------------
	const float Before = GApplyProbeCVar.GetValueOnGameThread();
	FFPMApplyLine Line1;
	if (!ApplyCVarLever(Scale, Owner, TEXT("apply self-test"), Line1)
	    || Line1.Outcome != EFPMApplyOutcome::Written)
	{
		Fail(FString::Printf(TEXT("(1) the known-positive lever did not write: %s -- %s"),
			LexToString(Line1.Outcome), *Line1.Why));
	}
	const float AfterFirst = GApplyProbeCVar.GetValueOnGameThread();
	if (bOk && FMath::IsNearlyEqual(AfterFirst, Before))
	{
		Fail(FString::Printf(
			TEXT("(1) the cvar did not move: %s before, %s after. A hold that changes nothing reads "
			     "exactly like a hold that worked."),
			*FPMFormatLeverValue(Before), *FPMFormatLeverValue(AfterFirst)));
	}

	// ---- (2) THE ANTI-RATCHET, with its own coverage guard. ----------------------------------------
	// Apply the SAME lever again without releasing. The captured baseline is not re-read, so the
	// landing must be identical. A live-read implementation would have read AfterFirst and scaled it
	// again -- and if that number happens to coincide with the correct one, this check proves nothing
	// and says so rather than passing.
	FFPMApplyLine Line2;
	ApplyCVarLever(Scale, Owner, TEXT("apply self-test, second pass"), Line2);
	if (Line2.Outcome != EFPMApplyOutcome::Written)
	{
		// Without this, two FAILED applies would leave the cvar identical and check (2) would read
		// that as "no ratchet". A check whose pass condition is also satisfied by doing nothing is
		// not a check.
		Fail(FString::Printf(TEXT("(2) the second pass did not write (%s), so the comparison below "
		                          "would pass on two failures rather than on a stable baseline"),
			LexToString(Line2.Outcome)));
	}
	const float AfterSecond = GApplyProbeCVar.GetValueOnGameThread();
	const float LiveReadWouldGive = AfterFirst * 2.0f;

	if (FMath::IsNearlyEqual(LiveReadWouldGive, AfterFirst))
	{
		Fail(FString::Printf(
			TEXT("(2) NO COVERAGE: a live-read implementation would have produced %s, the same value "
			     "as the correct one, so this check cannot tell them apart. Choose a fixture baseline "
			     "or scale where they differ."),
			*FPMFormatLeverValue(LiveReadWouldGive)));
	}
	else if (!FMath::IsNearlyEqual(AfterSecond, AfterFirst))
	{
		Fail(FString::Printf(
			TEXT("(2) RATCHET: applying the same lever twice moved it from %s to %s. The baseline is "
			     "being re-read instead of taken from the once-only capture."),
			*FPMFormatLeverValue(AfterFirst), *FPMFormatLeverValue(AfterSecond)));
	}

	// ---- (1b) KNOWN-POSITIVE: a release really releases. -------------------------------------------
	FPMCVarWriter::Get().ReleaseOwner(Owner);
	const float AfterRelease = GApplyProbeCVar.GetValueOnGameThread();
	if (!FMath::IsNearlyEqual(AfterRelease, Before))
	{
		Fail(FString::Printf(
			TEXT("(1b) release did not restore: %s before, %s after release. The engine's tagged "
			     "Unset is the whole zero-residue mechanism, so this failing means uninstall leaves "
			     "residue."),
			*FPMFormatLeverValue(Before), *FPMFormatLeverValue(AfterRelease)));
	}

	// ---- (3) KNOWN-NEGATIVE: availability is obeyed. -----------------------------------------------
	FFPMApplyLine Line3;
	const bool bWroteAbsent = ApplyCVarLever(Absent, Owner, TEXT("apply self-test"), Line3);
	if (bWroteAbsent || Line3.Outcome != EFPMApplyOutcome::SkippedUnavailable)
	{
		Fail(FString::Printf(
			TEXT("(3) a lever whose cvar does not exist was not skipped as unavailable: %s. A gate "
			     "that only ever passes is the defect this project has shipped four times."),
			LexToString(Line3.Outcome)));
	}

	// ---- (4) THE GROUP FILTER, both directions. ----------------------------------------------------
	if (FPMClassifyGroupMember(FFPMStageTables::ForbiddenGICVarName())
	    != EFPMGroupMemberExclusion::ForbiddenGICVar)
	{
		Fail(TEXT("(4) FPMClassifyGroupMember did not exclude the GI kill switch by name. The floor "
		          "law is enforced at the group expansion, so this failing means the ladder can reach "
		          "r.Lumen.DiffuseIndirect.Allow."));
	}
	if (FPMClassifyGroupMember(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"))
	    != EFPMGroupMemberExclusion::None)
	{
		Fail(TEXT("(4) the mirror half failed: an ordinary group member was excluded. A filter that "
		          "excludes everything produces the same clean ladder as a correct one."));
	}

	// ---- (5) RESOLUTION IS REFUSED, NAMED. ---------------------------------------------------------
	{
		FFPMSteerDecision ResDecision;
		ResDecision.Action = EFPMSteerAction::ResolutionDown;
		ResDecision.Tier = EFPMStageTier::Resolution;
		const FFPMApplyResult ResResult = Execute(ResDecision);
		if (ResResult.bExecuted || ResResult.Refusal.IsEmpty())
		{
			Fail(TEXT("(5) a ResolutionDown decision was not refused with a reason. Reporting success "
			          "for a lever this file does not carry would tell the walk it reached a floor "
			          "nothing moved toward."));
		}
	}

	// ---- (6) NOTHING LEFT BEHIND. ------------------------------------------------------------------
	FPMCVarWriter::Get().ReleaseOwner(Owner);
	TArray<FString> HeldAfter;
	FPMCVarWriter::Get().GetHeldCVars(HeldAfter);
	FString Delta;
	if (!FPMStageInvariants::SameNameSet(HeldBefore, HeldAfter, Delta))
	{
		Fail(FString::Printf(
			TEXT("(6) the self-test leaked holds: %s. A proof that leaves residue has disproved the "
			     "thing it was checking."), *Delta));
	}

	SessionWrites = SavedWrites;
	SessionReleases = SavedReleases;
	SessionRefusals = SavedRefusals;

	// ★ THE COVERAGE THIS PROOF DOES NOT HAVE, said out loud rather than left as a clean-looking pass.
	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] apply self-test coverage: the WriterRefused path is NOT exercised here and cannot "
		     "be. Every refusal the writer makes is caught earlier -- a missing cvar by the capability "
		     "probe, an sg.* or US_*-backed name by the registry at registration -- so no reachable "
		     "fixture gets that far. FPMCVarWriter::SelfTest is what proves the writer refuses."));

	return bOk;
}

// ------------------------------------------------------------------------------------------------
// Reporting
// ------------------------------------------------------------------------------------------------

void FFPMStageApply::PrintResult(const FFPMApplyResult& Result, FOutputDevice& Ar)
{
	if (!Result.bExecuted)
	{
		Ar.Logf(TEXT("[FPM] apply %s %s: NOT EXECUTED -- %s"),
			LexToString(Result.Action), LexToString(Result.Tier), *Result.Refusal);
		return;
	}

	Ar.Logf(TEXT("[FPM] apply %s %s: %d written, %d released, %d skipped"),
		LexToString(Result.Action), LexToString(Result.Tier),
		Result.Written, Result.Released, Result.Skipped);

	for (const FFPMApplyLine& Line : Result.Lines)
	{
		// The group prefix is what tells a reader that a member cvar moved because a GROUP stepped,
		// rather than because somebody added a hand lever with that name to the tier.
		const FString Label = Line.FromGroup.IsEmpty()
			? Line.CVarName
			: FString::Printf(TEXT("%s/%s"), *Line.FromGroup, *Line.CVarName);

		Ar.Logf(TEXT("[FPM]   %-58s %-14s %10s -> %-10s %s"),
			*Label, LexToString(Line.Outcome),
			Line.ObservedBefore.IsEmpty() ? TEXT("-") : *Line.ObservedBefore,
			Line.Target.IsEmpty() ? TEXT("-") : *Line.Target,
			*Line.Why);
	}

	// ★ THE FILTER'S OWN DENOMINATOR. A group step that excluded nothing and a group step that
	// expanded to nothing both write no forbidden member, and only this line tells them apart.
	Ar.Logf(TEXT("[FPM]   group members examined %d; excluded %d for the GI floor law, %d as "
	             "US_*-backed."),
		Result.GroupMembersExamined, Result.ExcludedGIKillSwitch, Result.ExcludedUserSetting);
}

void FFPMStageApply::ReportNow(FOutputDevice& Ar) const
{
	FPMReportGate Gate(Ar, TEXT("FPM.Stage.Apply.Report"));
	if (Gate.IsRefused()) { return; }
	FPMScopedConsoleEcho Echo(&Ar);

	if (!bSelfTestPassed)
	{
		Ar.Logf(TEXT("[FPM] apply pass: the self-test has NOT passed, so this report would be a table "
		             "of numbers nobody should trust. The boot log carries the failure."));
		return;
	}

	Ar.Logf(TEXT("[FPM] apply pass -- the one route from a decision to a console-variable write."));
	Ar.Logf(TEXT("[FPM]   session: %d write(s), %d release(s), %d refusal(s)."),
		SessionWrites, SessionReleases, SessionRefusals);

	int32 HeldTiers = 0;
	for (int32 I = 1; I < static_cast<int32>(EFPMStageTier::Count); ++I)
	{
		const EFPMStageTier Tier = static_cast<EFPMStageTier>(I);
		if (!bTierHeld[I]) { continue; }
		++HeldTiers;
		Ar.Logf(TEXT("[FPM]   HELD %-4s owner '%s'"), LexToString(Tier), *OwnerFor(Tier).ToString());
	}
	if (HeldTiers == 0)
	{
		Ar.Logf(TEXT("[FPM]   no tier is held. The ladder is at vanilla."));
	}

	// ⚠ WHAT IS STILL MISSING, named here rather than left for a reader to infer from silence.
	Ar.Logf(TEXT("[FPM]   nothing DRIVES this yet. A governor tick needs three things that are not "
	             "built: the hard-drop bind attribution (section 3.6), the resolution executor "
	             "(section 8), and a bench profile (section 4) -- without a profile the walk refuses "
	             "every stage tier by design, so an automatic driver today would idle in a named "
	             "block. Use FPM.Stage.Apply to exercise this by hand."));
}

void FFPMStageApply::ApplyFromConsole(const TArray<FString>& Args, FOutputDevice& Ar)
{
	FPMReportGate Gate(Ar, TEXT("FPM.Stage.Apply"));
	if (Gate.IsRefused()) { return; }
	FPMScopedConsoleEcho Echo(&Ar);

	if (Args.Num() == 0)
	{
		Ar.Logf(TEXT("[FPM] usage: FPM.Stage.Apply <tier> <engage|release>, or "
		             "FPM.Stage.Apply release-all. Tiers: B1..B6, K1, K2, K3, K4g, K4f."));
		Ar.Logf(TEXT("[FPM] this WRITES, through FPMCVarWriter, at 0x07, with a hold that FPM.Off "
		             "and a world change both release."));
		return;
	}

	if (Args[0].Equals(TEXT("release-all"), ESearchCase::IgnoreCase))
	{
		const int32 Dropped = ReleaseEverything(TEXT("FPM.Stage.Apply release-all"));
		Ar.Logf(TEXT("[FPM] released %d ladder hold(s)."), Dropped);
		return;
	}

	const EFPMStageTier Tier = ParseTier(Args[0]);
	if (Tier == EFPMStageTier::None)
	{
		Ar.Logf(TEXT("[FPM] '%s' is not a tier. Tiers: B1..B6, K1, K2, K3, K4g, K4f."), *Args[0]);
		return;
	}

	const bool bRelease = Args.Num() > 1 && Args[1].StartsWith(TEXT("rel"), ESearchCase::IgnoreCase);

	FFPMSteerDecision Decision;
	Decision.Tier = Tier;
	if (FPMIsBonusTier(Tier))
	{
		Decision.Action = bRelease ? EFPMSteerAction::DemoteBonus : EFPMSteerAction::PromoteBonus;
	}
	else
	{
		Decision.Action = bRelease ? EFPMSteerAction::ReleaseCut : EFPMSteerAction::EngageCut;
	}
	Decision.Reason = TEXT("typed at the console by the operator");

	const FFPMApplyResult Result = Execute(Decision);
	PrintResult(Result, Ar);

	Ar.Logf(TEXT("[FPM]   FPM.Changes lists every live hold; FPM.Off releases all of them."));
}

// ------------------------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------------------------

void FFPMStageApply::Arm()
{
	FMemory::Memzero(bTierHeld, sizeof(bTierHeld));
	SessionWrites = 0;
	SessionReleases = 0;
	SessionRefusals = 0;

	// ★ FIXTURES REGISTER AT ARM, NOT AT WORLD LOAD, and the reason is the probe pass. The registry
	// probes every lever once, at ITS OnWorldLoad, which runs before this fix's. A fixture registered
	// later would sit at Availability::Unknown for ever and the self-test would skip its own
	// known-positive while reporting a pass.
	{
		FFPMLeverDefinition Def;
		Def.Name = ApplyFixtureScaleKey();
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { ApplyProbeName() };
		Def.Policy = EFPMLeverPolicy::BaseScale;
		Def.BaselineSource = EFPMLeverBaselineSource::CapturedOnce;
		Def.Side = EFPMFixSide::NeverOnDedicatedServer;
		Def.Provenance = TEXT("apply-pass self-test fixture: proves a write writes and does not ratchet");
		Def.TierHint = TEXT("(fixture)");
		FFPMLeverRegistry::Get().RegisterWritable(MoveTemp(Def));
	}
	{
		FFPMLeverDefinition Def;
		Def.Name = ApplyFixtureAbsentKey();
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { ApplyKnownAbsentName() };
		Def.Policy = EFPMLeverPolicy::Absolute;
		Def.Side = EFPMFixSide::NeverOnDedicatedServer;
		Def.Provenance = TEXT("apply-pass self-test fixture: the known-negative for the availability gate");
		Def.TierHint = TEXT("(fixture)");
		FFPMLeverRegistry::Get().RegisterWritable(MoveTemp(Def));
	}

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] apply pass armed. This is the route a decision takes to a real cvar write: holds "
		     "at 0x07 through FPMCVarWriter, one owner per TIER IDENTITY, released by the engine's "
		     "tagged Unset. Nothing calls it automatically yet -- FPM.Stage.Apply drives it by hand "
		     "and FPM.Stage.Apply.Report says what a driver is still waiting for."));
}

void FFPMStageApply::OnWorldLoad(UWorld* World)
{
	// ⚠ A NEW WORLD VOIDS THE OLD LADDER. The previous world's holds describe a scene that no longer
	// exists, and leaving them would put gameplay values into a menu world -- the exact harm
	// EFPMLease::World was invented for and which nothing in this tree implements. Releasing here is
	// that guarantee, made out of a mechanism that exists.
	ReleaseEverything(TEXT("world load: the previous world's ladder position is void"));

	bSelfTestPassed = SelfTest();

	UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::Steering), LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] apply pass: self-test %s."), bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

void FFPMStageApply::Disarm()
{
	// The whole point of Disarm for a fix that writes cvars: OFF has to mean RELEASED.
	ReleaseEverything(TEXT("apply pass disarmed"));
	bSelfTestPassed = false;
}

// ------------------------------------------------------------------------------------------------
// Console surface
// ------------------------------------------------------------------------------------------------

static FAutoConsoleCommandWithArgsAndOutputDevice GFPMStageApplyCmd(
	TEXT("FPM.Stage.Apply"),
	TEXT("Engage or release ONE stage tier for real, through FPMCVarWriter. "
	     "Usage: FPM.Stage.Apply <tier> <engage|release>, or FPM.Stage.Apply release-all."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			FFPMStageApply::Get().ApplyFromConsole(Args, Ar);
		}));

static FAutoConsoleCommandWithOutputDevice GFPMStageApplyReportCmd(
	TEXT("FPM.Stage.Apply.Report"),
	TEXT("Which stage tiers the apply pass currently holds, its session counters, and what a "
	     "governor driver is still waiting for. Reads only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMStageApply::Get().ReportNow(Ar);
	}));

/**
 * ★ SECTION 5.1'S SECOND HALF: "every writing diagnostic registers a stop hook with the master
 * switch, so OFF means RELEASED".
 *
 * FPMCVarWriter::ReleaseAll already unsets every tagged hold, so the cvars themselves come back
 * without this. What does NOT come back without it is this class's own bookkeeping: after an OFF,
 * IsTierHeld would keep claiming tiers are held while nothing is. An inventory that lies is the one
 * thing this project refuses to ship, so the hook exists to correct the RECORD as well as the state.
 *
 * File-scope static, in the owning translation unit, for the reason FPMCVarProbe.cpp gives: the hook
 * must exist before StartupModule can ever reach the OFF branch.
 */
static const bool GFPMStageApplyStopHookRegistered = []
{
	FPMMasterSwitch::RegisterStopHook([]()
	{
		FFPMStageApply::Get().ReleaseEverything(TEXT("master switch OFF"));
	});
	return true;
}();
