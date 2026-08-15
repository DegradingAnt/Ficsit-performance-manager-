// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMLeverRegistry.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMUserSettingMap.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/OutputDevice.h"

const TCHAR* LexToString(const EFPMLeverPolicy Policy)
{
	switch (Policy)
	{
	case EFPMLeverPolicy::MaxOf:            return TEXT("MaxOf");
	case EFPMLeverPolicy::MinOf:            return TEXT("MinOf");
	case EFPMLeverPolicy::Absolute:         return TEXT("Absolute");
	case EFPMLeverPolicy::BaseScale:        return TEXT("BaseScale");
	case EFPMLeverPolicy::BaseDelta:        return TEXT("BaseDelta");
	case EFPMLeverPolicy::ScalabilityGroup: return TEXT("ScalabilityGroup");
	default:                                return TEXT("<unknown policy>");
	}
}

FString LexToString(const EFPMLeverCurrency Currency)
{
	if (Currency == EFPMLeverCurrency::None)
	{
		return TEXT("none");
	}
	TArray<FString> Parts;
	if (EnumHasAnyFlags(Currency, EFPMLeverCurrency::GpuMs))  { Parts.Add(TEXT("gpu_ms")); }
	if (EnumHasAnyFlags(Currency, EFPMLeverCurrency::VramMb)) { Parts.Add(TEXT("vram_mb")); }
	if (EnumHasAnyFlags(Currency, EFPMLeverCurrency::CpuMs))  { Parts.Add(TEXT("cpu_ms")); }
	return FString::Join(Parts, TEXT("+"));
}

const TCHAR* LexToString(const EFPMLeverAvailability Availability)
{
	switch (Availability)
	{
	case EFPMLeverAvailability::Unknown:   return TEXT("not yet probed");
	case EFPMLeverAvailability::Available: return TEXT("AVAILABLE");
	case EFPMLeverAvailability::Absent:    return TEXT("ABSENT");
	case EFPMLeverAvailability::Refused:   return TEXT("REFUSED");
	default:                               return TEXT("<unknown availability>");
	}
}

namespace
{
	/**
	 * A name no console variable can carry, used as the capability-probe classifier's known-
	 * negative. Same shape and same reason as FPMServerLevers.cpp's `KnownAbsentProbeName` --
	 * deliberately NOT in FPM's own cvar namespace, and deliberately a DIFFERENT string from that
	 * one, so `check_probe_name_single_site` never sees either literal at 4+ sites and the two
	 * self-tests stay independently readable.
	 */
	const TCHAR* KnownAbsentCVarName()
	{
		return TEXT("fpm.lever.registry.this.console.variable.must.never.exist");
	}

	/** The self-test's OWN registry-side owner name, for FPMCVarWriter::Hold/Release/ReleaseOwner. */
	FName SelfTestOwnerName()
	{
		return FName(TEXT("FPMLeverRegistry.SelfTest"));
	}
}

FFPMLeverRegistry& FFPMLeverRegistry::Get()
{
	static FFPMLeverRegistry Instance;
	return Instance;
}

// ------------------------------------------------------------------------------------------------
// Registration
// ------------------------------------------------------------------------------------------------

bool FFPMLeverRegistry::RefuseIfUnsafeToWrite(const FFPMLeverDefinition& Def, FString& OutReason) const
{
	// clause-2 mirror (FPMCVarWriter.cpp:22-23): no lever may name a scalability GROUP cvar
	// directly, whichever backing it declares. A ScalabilityGroup-backed lever's real writes are
	// its alias-table MEMBERS, resolved through GetAliasMembers -- never `sg.<Group>` itself.
	for (const FString& CVar : Def.CVarNames)
	{
		if (CVar.StartsWith(TEXT("sg."), ESearchCase::IgnoreCase))
		{
			OutReason = FString::Printf(
				TEXT("names '%s' directly -- FPMCVarWriter clause 2 refuses every sg.* write "
				     "unconditionally; a ScalabilityGroup lever drives its GROUP'S MEMBER cvars "
				     "(the alias table), never the group name itself"), *CVar);
			return true;
		}
	}

	// clause-6 mirror, LAW 1, STRUCTURALLY: a US_*-backed cvar cannot become a writable lever. This
	// is the suspenders to FPMCVarWriter's own belt (its clause 6 would ALSO refuse the write later)
	// -- a lever that fails here never reaches a live Hold() call in the first place.
	for (const FString& CVar : Def.CVarNames)
	{
		if (FPMUserSettingMap::IsBacked(*CVar))
		{
			OutReason = FString::Printf(
				TEXT("names '%s', which is US_*-backed (FPMUserSettingMap::IsBacked). "
				     "FGGameUserSettings serialises every mUserSettings entry on every save with NO "
				     "dirty gate -- a writable lever here would become the player's PERMANENT "
				     "setting and survive uninstall"), *CVar);
			return true;
		}
	}

	if (Def.Backing == EFPMLeverBacking::ScalabilityGroup && Def.GroupName.IsEmpty())
	{
		OutReason = TEXT("Backing is ScalabilityGroup but GroupName is empty -- nothing for the "
		                  "alias table to resolve");
		return true;
	}

	if (Def.Backing == EFPMLeverBacking::Cvar && Def.CVarNames.Num() == 0)
	{
		OutReason = TEXT("Backing is Cvar but CVarNames is empty -- nothing to write and nothing "
		                  "to probe");
		return true;
	}

	// LAW 3, STRUCTURALLY: a policy that COMPARES against a baseline must declare where that
	// baseline comes from. EFPMLeverBaselineSource offers no "read it live" option at all -- see
	// its own doc comment -- so this check only ever catches "forgot to declare one", never "chose
	// the unsafe one", because the unsafe one is not a value that exists to choose.
	const bool bNeedsBaseline =
		Def.Policy == EFPMLeverPolicy::MaxOf || Def.Policy == EFPMLeverPolicy::MinOf ||
		Def.Policy == EFPMLeverPolicy::BaseScale || Def.Policy == EFPMLeverPolicy::BaseDelta;
	if (bNeedsBaseline && Def.BaselineSource == EFPMLeverBaselineSource::NotApplicable)
	{
		OutReason = FString::Printf(
			TEXT("policy %s compares against a baseline but BaselineSource is NotApplicable -- "
			     "declare ShippedTable or CapturedOnce (Law 3)"), LexToString(Def.Policy));
		return true;
	}

	return false;
}

namespace
{
	/**
	 * ⚠ SEVERITY, NOT SUPPRESSION. Every refusal in this file routes through here.
	 *
	 * The message is IDENTICAL either way and the caller still records it in RefusedRegistrations,
	 * so nothing is hidden and the evidence that a guard fired always survives. Only the VERBOSITY
	 * moves, and only for a self-test fixture, whose refusal is the EXPECTED result. A PRODUCTION
	 * lever refused anywhere in this file is a real defect and stays at Error.
	 *
	 * WHY IT EXISTS. The self-test feeds this registry deliberately-bad levers so the guards can be
	 * seen to fire. Every refusal logged at Error, UAT counts Error lines, and on 2026-08-15 that
	 * turned `Failure - 3 error(s)` into Error_UnknownCookFailure: A PASSING SELF-TEST FAILED THE
	 * BUILD.
	 *
	 * WHY IT IS A HELPER AND NOT AN INLINE BRANCH. The first fix branched at ONE site and left the
	 * identical shape at four others, twelve lines away. A review found them. One chokepoint means a
	 * new refusal cannot be added with the old shape by accident, and the message cannot drift
	 * between two copies of itself.
	 */
	void LogLeverRefusal(bool bIsSelfTestFixture, const FString& Line)
	{
		if (bIsSelfTestFixture)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] lever registry: %s [self-test fixture: this refusal is the PASS]"), *Line);
			return;
		}
		UE_LOG(LogFicsitsPerformanceManager, Error, TEXT("[FPM] lever registry: %s"), *Line);
	}

	/** The fixture test on a bare name, for the refusal paths that have no FFPMLeverDefinition yet. */
	bool IsSelfTestName(const FName LeverName)
	{
		return LeverName.ToString().StartsWith(FPMLeverSelfTestPrefix(), ESearchCase::CaseSensitive);
	}
}

const FFPMLeverDefinition* FFPMLeverRegistry::RegisterWritable(FFPMLeverDefinition Definition)
{
	// M2: derived, never hand-set. See FPMLeverSelfTestPrefix's comment in FPMLeverTypes.h.
	// ★ DERIVED BEFORE THE REFUSAL, not after. It used to be set only on the SUCCESS path below,
	// which meant a refused fixture was indistinguishable from a refused production lever.
	Definition.bIsSelfTestFixture = IsSelfTestName(Definition.Name);

	FString Reason;
	if (RefuseIfUnsafeToWrite(Definition, Reason))
	{
		const FString Line = FString::Printf(TEXT("'%s' refused as WRITABLE: %s"),
			*Definition.Name.ToString(), *Reason);
		LogLeverRefusal(Definition.bIsSelfTestFixture, Line);
		RefusedRegistrations.Add(Line);
		return nullptr;
	}

	Definition.bWritable = true;
	Definition.Availability = EFPMLeverAvailability::Unknown;
	const FName Key = Definition.Name;
	FFPMLeverDefinition& Stored = Levers.Add(Key, MoveTemp(Definition));

	UE_LOG(LogFicsitsPerformanceManager, Verbose,
		TEXT("[FPM] lever registry: '%s' registered WRITABLE (%s backing, %s policy)"),
		*Stored.Name.ToString(),
		Stored.Backing == EFPMLeverBacking::Cvar ? TEXT("cvar") : TEXT("scalability-group"),
		LexToString(Stored.Policy));
	return &Stored;
}

const FFPMLeverDefinition* FFPMLeverRegistry::RegisterReadOnly(FFPMLeverDefinition Definition)
{
	// ★ DERIVED BEFORE THE REFUSAL, same as RegisterWritable. This function carried the identical
	// defect twelve lines from the one that was fixed first, and a review caught it. It is inert
	// today only because RegisterReadOnly currently has no call sites, which is exactly the kind of
	// "harmless" that stops being harmless the day someone calls it.
	Definition.bIsSelfTestFixture = IsSelfTestName(Definition.Name);

	if (Definition.Backing == EFPMLeverBacking::Cvar && Definition.CVarNames.Num() == 0)
	{
		const FString Line = FString::Printf(
			TEXT("'%s' refused as READ-ONLY: Backing is Cvar but CVarNames is empty"),
			*Definition.Name.ToString());
		LogLeverRefusal(Definition.bIsSelfTestFixture, Line);
		RefusedRegistrations.Add(Line);
		return nullptr;
	}

	Definition.bWritable = false;
	Definition.Availability = EFPMLeverAvailability::Unknown;
	const FName Key = Definition.Name;
	FFPMLeverDefinition& Stored = Levers.Add(Key, MoveTemp(Definition));

	UE_LOG(LogFicsitsPerformanceManager, Verbose,
		TEXT("[FPM] lever registry: '%s' registered READ-ONLY"), *Stored.Name.ToString());
	return &Stored;
}

int32 FFPMLeverRegistry::CountSelfTestFixtures() const
{
	int32 Count = 0;
	for (const auto& Pair : Levers)
	{
		if (Pair.Value.bIsSelfTestFixture) { ++Count; }
	}
	return Count;
}

int32 FFPMLeverRegistry::CountProduction() const
{
	return Levers.Num() - CountSelfTestFixtures();
}

const FFPMLeverDefinition* FFPMLeverRegistry::Find(const FName LeverName) const
{
	return Levers.Find(LeverName);
}

// ------------------------------------------------------------------------------------------------
// Capability probing (§3.12) -- OnWorldLoad only, never per-tick, never at Arm (see class doc).
// ------------------------------------------------------------------------------------------------

void FFPMLeverRegistry::ProbeOne(FFPMLeverDefinition& Def) const
{
	if (Def.CapabilityProbe)
	{
		Def.Availability = Def.CapabilityProbe()
			? EFPMLeverAvailability::Available
			: EFPMLeverAvailability::Absent;
	}
	else if (Def.Backing == EFPMLeverBacking::Cvar)
	{
		// No probe supplied -- fall back to the minimum §3.12 asks of every lever: does the
		// console variable exist at all. bTrackFrequentCalls=false for the same reason
		// FPMServerLevers.cpp uses it here -- a miss is an expected FINDING, not a fault to warn
		// the engine's own counters about.
		bool bAllExist = true;
		for (const FString& CVar : Def.CVarNames)
		{
			if (!IConsoleManager::Get().FindConsoleVariable(*CVar, false))
			{
				bAllExist = false;
				break;
			}
		}
		Def.Availability = bAllExist ? EFPMLeverAvailability::Available : EFPMLeverAvailability::Absent;
	}
	else
	{
		// ScalabilityGroup with no probe: availability is "does the alias table have at least one
		// tier for this group", which RefreshAliasTable populates before this runs.
		const TMap<int32, TArray<FString>>* Tiers = AliasTable.Find(Def.GroupName);
		Def.Availability = (Tiers && Tiers->Num() > 0)
			? EFPMLeverAvailability::Available
			: EFPMLeverAvailability::Absent;
	}

	if (Def.Availability == EFPMLeverAvailability::Absent)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] lever '%s' ABSENT on this build (%s)"),
			*Def.Name.ToString(), Def.Provenance.IsEmpty() ? TEXT("no provenance recorded") : *Def.Provenance);
	}
}

// ------------------------------------------------------------------------------------------------
// The alias table -- the live BaseScalability.ini, tiers 0-3, never a raw file re-parse.
// ------------------------------------------------------------------------------------------------

void FFPMLeverRegistry::RefreshAliasTable()
{
	TSet<FString> Groups;
	for (const auto& Pair : Levers)
	{
		if (Pair.Value.Backing == EFPMLeverBacking::ScalabilityGroup && !Pair.Value.GroupName.IsEmpty())
		{
			Groups.Add(Pair.Value.GroupName);
		}
	}

	AliasTable.Reset();
	if (!GConfig)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry: GConfig is null -- cannot build the alias table"));
		return;
	}

	for (const FString& Group : Groups)
	{
		TMap<int32, TArray<FString>> PerTier;
		for (int32 Tier = 0; Tier <= 3; ++Tier)
		{
			const FString Section = FString::Printf(TEXT("%s@%d"), *Group, Tier);
			if (const FConfigSection* Sec = GConfig->GetSection(*Section, false, GScalabilityIni))
			{
				TArray<FString> Members;
				Members.Reserve(Sec->Num());
				for (const auto& KVP : *Sec)
				{
					Members.Add(KVP.Key.ToString());
				}
				PerTier.Add(Tier, MoveTemp(Members));
			}
		}

		if (PerTier.Num() == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] lever registry: alias table found NO tiers for scalability group '%s' "
				     "in the live BaseScalability.ini -- every lever backed by it will probe ABSENT"),
				*Group);
		}
		AliasTable.Add(Group, MoveTemp(PerTier));
	}
}

const TArray<FString>* FFPMLeverRegistry::GetAliasMembers(const FString& GroupName, const int32 Tier) const
{
	const TMap<int32, TArray<FString>>* Tiers = AliasTable.Find(GroupName);
	return Tiers ? Tiers->Find(Tier) : nullptr;
}

// ------------------------------------------------------------------------------------------------
// The read side, honestly split. See the class doc comment.
// ------------------------------------------------------------------------------------------------

bool FFPMLeverRegistry::GetOurHold(const FName LeverName, FString& OutValue) const
{
	const FFPMLeverDefinition* Def = Levers.Find(LeverName);
	if (!Def || Def->CVarNames.Num() == 0)
	{
		return false;
	}

	// Multi-cvar levers report their FIRST cvar's hold as the headline value; a caller wanting
	// every member individually reads FPMCVarWriter::GetHolds() directly.
	TArray<FPMCVarWriter::FHoldView> Holds;
	FPMCVarWriter::Get().GetHolds(Holds);
	for (const FPMCVarWriter::FHoldView& Hold : Holds)
	{
		if (Hold.CVar == Def->CVarNames[0])
		{
			OutValue = Hold.Value;
			return true;
		}
	}
	return false;
}

bool FFPMLeverRegistry::GetCVarRequestedValue(const FName LeverName, FString& OutValue) const
{
	const FFPMLeverDefinition* Def = Levers.Find(LeverName);
	if (!Def || Def->CVarNames.Num() == 0)
	{
		return false;
	}

	const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Def->CVarNames[0], false);
	if (!Var)
	{
		return false;
	}
	OutValue = Var->GetString();
	return true;
}

// ------------------------------------------------------------------------------------------------
// Law 3's enforcement point.
// ------------------------------------------------------------------------------------------------

bool FFPMLeverRegistry::CaptureBaselineOnce(const FName LeverName)
{
	// ★ ALL FOUR refusal paths below route through LogLeverRefusal. A review found that the first
	// fix branched only the ratchet guard and left these three logging unconditional Error, which
	// is the same shape that cost a cook. None of the five current fixtures reaches them, so nothing
	// was broken today, but "currently unreachable" is not a fix.
	FFPMLeverDefinition* Def = Levers.Find(LeverName);
	if (!Def)
	{
		// No Def to read the flag from, so derive it from the NAME. A lookup that misses cannot
		// tell you what it was looking for, except by its name.
		LogLeverRefusal(IsSelfTestName(LeverName), FString::Printf(
			TEXT("CaptureBaselineOnce('%s') -- unknown lever"), *LeverName.ToString()));
		return false;
	}

	if (Def->BaselineSource != EFPMLeverBaselineSource::CapturedOnce)
	{
		LogLeverRefusal(Def->bIsSelfTestFixture, FString::Printf(
			TEXT("CaptureBaselineOnce('%s') refused -- BaselineSource is not CapturedOnce. "
			     "ShippedTable levers take their baseline from a compiled table (not this call); "
			     "NotApplicable levers have no baseline to capture at all."),
			*LeverName.ToString()));
		return false;
	}

	if (Def->bBaselineCaptured)
	{
		// Idempotent -- the "once" guarantee made observable. NOT a re-read.
		return true;
	}

	if (Def->CVarNames.Num() == 0)
	{
		LogLeverRefusal(Def->bIsSelfTestFixture, FString::Printf(
			TEXT("CaptureBaselineOnce('%s') refused -- no cvar to read"), *LeverName.ToString()));
		return false;
	}
	const FString& CVarName = Def->CVarNames[0];

	// ★ THE RATCHET GUARD. Refuse if we ALREADY hold this cvar: reading it now risks reading our
	// own prior write back as if it were the baseline, which is the exact measured failure Law 3
	// exists to prevent. Capture before the first Hold, or never.
	if (FPMCVarWriter::Get().IsHeld(*CVarName))
	{
		// The self-test drives this guard ON PURPOSE, by holding a probe cvar and then asking for a
		// baseline. That refusal is the PASS. A PRODUCTION lever here is a real ordering defect.
		// Message built ONCE, so the two severities cannot drift apart -- a review flagged the
		// earlier version for carrying two verbatim copies of it.
		LogLeverRefusal(Def->bIsSelfTestFixture, FString::Printf(
			TEXT("REFUSING to capture baseline for '%s' -- FPM already holds '%s'. Reading it now "
			     "would risk reading our OWN prior write back as the baseline (the ratchet "
			     "failure). Capture baselines before the first Hold, never after."),
			*LeverName.ToString(), *CVarName));
		return false;
	}

	const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*CVarName, false);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] lever registry: CaptureBaselineOnce('%s') found no console variable '%s' -- "
			     "leaving uncaptured"), *LeverName.ToString(), *CVarName);
		return false;
	}

	Def->CapturedBaselineValue = Var->GetString();
	Def->bBaselineCaptured = true;
	return true;
}

// ------------------------------------------------------------------------------------------------
// Self-test fixtures. Every known-positive/known-negative pair below is a REAL, already-verified
// fact about this codebase or this engine install -- none is invented for the test.
// ------------------------------------------------------------------------------------------------

void FFPMLeverRegistry::RegisterSelfTestLevers()
{
	// Known-negative for the hazard (a SAFE lever): FPM's own scratch probe cvar. Guaranteed
	// present (FPMCVarWriter registers it) and guaranteed NOT US_*-backed. Registration must
	// SUCCEED -- this is also the "correct lever wrongly refused" mirror check.
	{
		FFPMLeverDefinition Def;
		Def.Name = FName(TEXT("__SelfTest.NonBacked"));
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { FPMCVarWriter::SelfTestProbeName() };
		Def.Policy = EFPMLeverPolicy::MaxOf;
		Def.BaselineSource = EFPMLeverBaselineSource::CapturedOnce;
		Def.Provenance = TEXT("self-test fixture -- not a real tunable");
		Def.CapabilityProbe = []() {
			return IConsoleManager::Get().FindConsoleVariable(FPMCVarWriter::SelfTestProbeName(), false) != nullptr;
		};
		RegisterWritable(MoveTemp(Def));
	}

	// A second definition over the SAME cvar, fresh (never captured), used only by the baseline
	// held-cvar refusal check below -- a separate registry key so the first fixture's own capture
	// does not interfere with this one's "must still be uncaptured" precondition.
	{
		FFPMLeverDefinition Def;
		Def.Name = FName(TEXT("__SelfTest.HeldCvar"));
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { FPMCVarWriter::SelfTestProbeName() };
		Def.Policy = EFPMLeverPolicy::MaxOf;
		Def.BaselineSource = EFPMLeverBaselineSource::CapturedOnce;
		Def.Provenance = TEXT("self-test fixture -- not a real tunable");
		RegisterWritable(MoveTemp(Def));
	}

	// Known-positive for the hazard (an UNSAFE lever): t.MaxFPS, confirmed US_MaxFPS-backed
	// (FPMUserSettingMap.h:30-32 -- StrId "t.MaxFPS", UseCVar true). Registration must be REFUSED.
	{
		FFPMLeverDefinition Def;
		Def.Name = FName(TEXT("__SelfTest.USBacked"));
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { TEXT("t.MaxFPS") };
		Def.Policy = EFPMLeverPolicy::Absolute;
		Def.Provenance = TEXT("self-test fixture -- not a real tunable");
		RegisterWritable(MoveTemp(Def));
	}

	// Known-positive for the clause-2 hazard: a real vanilla sg.* name. Registration must be
	// REFUSED regardless of US_* status.
	{
		FFPMLeverDefinition Def;
		Def.Name = FName(TEXT("__SelfTest.ScalabilityGroupDirect"));
		Def.Backing = EFPMLeverBacking::Cvar;
		Def.CVarNames = { TEXT("sg.ViewDistanceQuality") };
		Def.Policy = EFPMLeverPolicy::Absolute;
		Def.Provenance = TEXT("self-test fixture -- not a real tunable");
		RegisterWritable(MoveTemp(Def));
	}

	// The alias table's own fixture: a real ScalabilityGroup lever over GlobalIlluminationQuality,
	// verified byte-for-byte against BaseScalability.ini:263-338 this session.
	{
		FFPMLeverDefinition Def;
		Def.Name = FName(TEXT("__SelfTest.GIAlias"));
		Def.Backing = EFPMLeverBacking::ScalabilityGroup;
		Def.GroupName = TEXT("GlobalIlluminationQuality");
		Def.Policy = EFPMLeverPolicy::ScalabilityGroup;
		Def.Provenance = TEXT("self-test fixture -- not a real tunable");
		RegisterWritable(MoveTemp(Def));
	}
}

bool FFPMLeverRegistry::SelfTest()
{
	bool bOk = true;

	// (1) US_* / clause-2 refusal classifier, BOTH directions.
	const FFPMLeverDefinition* Safe = Find(FName(TEXT("__SelfTest.NonBacked")));
	const FFPMLeverDefinition* Backed = Find(FName(TEXT("__SelfTest.USBacked")));
	const FFPMLeverDefinition* GroupDirect = Find(FName(TEXT("__SelfTest.ScalabilityGroupDirect")));
	if (!Safe || Backed || GroupDirect)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry self-test FAILED (refusal classifier): safe lever %s "
			     "(expected registered), US_*-backed lever %s (expected refused), direct sg.* "
			     "lever %s (expected refused)"),
			Safe ? TEXT("registered") : TEXT("MISSING"),
			Backed ? TEXT("REGISTERED") : TEXT("refused"),
			GroupDirect ? TEXT("REGISTERED") : TEXT("refused"));
		bOk = false;
	}

	// (2) Capability-probe classifier, both directions -- present cvar vs a name that cannot exist.
	const bool bProbePositive = IConsoleManager::Get().FindConsoleVariable(FPMCVarWriter::SelfTestProbeName(), false) != nullptr;
	const bool bProbeNegative = IConsoleManager::Get().FindConsoleVariable(KnownAbsentCVarName(), false) != nullptr;
	if (!bProbePositive || bProbeNegative)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry self-test FAILED (capability probe): known-present cvar '%s' "
			     "was %s (expected FOUND), known-absent name '%s' was %s (expected NOT found)"),
			FPMCVarWriter::SelfTestProbeName(), bProbePositive ? TEXT("found") : TEXT("NOT FOUND"),
			KnownAbsentCVarName(), bProbeNegative ? TEXT("FOUND") : TEXT("not found"));
		bOk = false;
	}

	// (3) Alias table, both directions -- a real member cvar at a real tier vs a group that does
	// not exist. RefreshAliasTable needs the GIAlias fixture already registered (it is, above).
	RefreshAliasTable();
	const TArray<FString>* GITier2 = GetAliasMembers(TEXT("GlobalIlluminationQuality"), 2);
	const bool bAliasPositive = GITier2 != nullptr && GITier2->ContainsByPredicate(
		[](const FString& S) { return S.Equals(TEXT("r.Lumen.DiffuseIndirect.Allow"), ESearchCase::IgnoreCase); });
	const TArray<FString>* NoSuchGroup = GetAliasMembers(TEXT("__SelfTest.GroupThatDoesNotExist"), 2);
	if (!bAliasPositive || NoSuchGroup != nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry self-test FAILED (alias table): GlobalIlluminationQuality@2 "
			     "%s r.Lumen.DiffuseIndirect.Allow (expected CONTAINS, verified against "
			     "BaseScalability.ini:284 this session), and a nonexistent group returned %s "
			     "(expected nullptr)"),
			bAliasPositive ? TEXT("contains") : TEXT("DOES NOT CONTAIN"),
			NoSuchGroup ? TEXT("a table") : TEXT("nullptr"));
		bOk = false;
	}

	// (4) The anti-ratchet baseline guard, both directions.
	const bool bFirstCapture = CaptureBaselineOnce(FName(TEXT("__SelfTest.NonBacked")));
	const FString FirstValue = Safe ? Safe->CapturedBaselineValue : FString();
	const bool bSecondCapture = CaptureBaselineOnce(FName(TEXT("__SelfTest.NonBacked")));
	const bool bIdempotent = Safe && Safe->CapturedBaselineValue == FirstValue;

	FPMCVarWriter::Get().Hold(SelfTestOwnerName(), FPMCVarWriter::SelfTestProbeName(), TEXT("1"),
		TEXT("lever registry self-test -- proving CaptureBaselineOnce refuses a held cvar"));
	const bool bHeldRefused = !CaptureBaselineOnce(FName(TEXT("__SelfTest.HeldCvar")));
	FPMCVarWriter::Get().Release(SelfTestOwnerName(), FPMCVarWriter::SelfTestProbeName());

	if (!bFirstCapture || !bSecondCapture || !bIdempotent || !bHeldRefused)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry self-test FAILED (baseline anti-ratchet): first capture %s, "
			     "second (idempotent) capture %s, value stable %s, held-cvar capture %s (expected "
			     "REFUSED)"),
			bFirstCapture ? TEXT("ok") : TEXT("FAILED"), bSecondCapture ? TEXT("ok") : TEXT("FAILED"),
			bIdempotent ? TEXT("ok") : TEXT("FAILED"), bHeldRefused ? TEXT("refused") : TEXT("SUCCEEDED"));
		bOk = false;
	}

	// (5) ★ INVARIANT: A BASELINE COMES FROM A DECLARED SAFE SOURCE, NEVER FROM A LIVE READ.
	//
	// Law 3 in this project's own words: a live read may be FPM's own earlier write coming back, so a
	// baseline taken that way RATCHETS -- each cycle starts from where the last one left off and the
	// player's own setting is walked away from them. Release is the moment that matters, because the
	// value a release restores IS the baseline.
	//
	// Three halves, and the third is the one that can rot quietly:
	//   (a) the enum offers no live-read option at all. Enumerated here so the day somebody adds one,
	//       this count changes and this check fails rather than the law becoming a comment.
	//   (b) the refusal classifier says NO to a baseline-comparing policy with no declared source and
	//       YES to the same definition with one. Both directions, on a real cvar name.
	//   (c) every WRITABLE lever now in the registry declares a safe source. This is a sweep with a
	//       printed denominator, because a sweep over zero levers passes.
	{
		// (a)
		constexpr int32 ExpectedBaselineSourceCount = 3;   // NotApplicable, ShippedTable, CapturedOnce
		const bool bEnumUnchanged =
			static_cast<int32>(EFPMLeverBaselineSource::NotApplicable) == 0
			&& static_cast<int32>(EFPMLeverBaselineSource::ShippedTable) == 1
			&& static_cast<int32>(EFPMLeverBaselineSource::CapturedOnce) == ExpectedBaselineSourceCount - 1;

		// (b) BOTH DIRECTIONS on the refusal classifier. The known-positive is a definition that must
		// be refused; the known-negative is the same definition made safe, which must be accepted.
		FFPMLeverDefinition Unsafe;
		Unsafe.Name = FName(TEXT("__SelfTest.BaselineUndeclared"));
		Unsafe.Backing = EFPMLeverBacking::Cvar;
		Unsafe.CVarNames = { FPMCVarWriter::SelfTestProbeName() };
		Unsafe.Policy = EFPMLeverPolicy::BaseScale;
		Unsafe.BaselineSource = EFPMLeverBaselineSource::NotApplicable;
		FString UnsafeWhy;
		const bool bUnsafeRefused = RefuseIfUnsafeToWrite(Unsafe, UnsafeWhy);

		FFPMLeverDefinition Safe2 = Unsafe;
		Safe2.BaselineSource = EFPMLeverBaselineSource::CapturedOnce;
		FString SafeWhy;
		const bool bSafeRefused = RefuseIfUnsafeToWrite(Safe2, SafeWhy);

		// (c)
		int32 WritableSwept = 0;
		int32 NeedingBaseline = 0;
		int32 Undeclared = 0;
		int32 ClaimingShippedTable = 0;
		for (const auto& Pair : Levers)
		{
			const FFPMLeverDefinition& Def = Pair.Value;
			if (!Def.bWritable) { continue; }
			++WritableSwept;
			const bool bNeeds =
				Def.Policy == EFPMLeverPolicy::MaxOf || Def.Policy == EFPMLeverPolicy::MinOf ||
				Def.Policy == EFPMLeverPolicy::BaseScale || Def.Policy == EFPMLeverPolicy::BaseDelta;
			if (!bNeeds) { continue; }
			++NeedingBaseline;
			if (Def.BaselineSource == EFPMLeverBaselineSource::NotApplicable) { ++Undeclared; }
			if (Def.BaselineSource == EFPMLeverBaselineSource::ShippedTable) { ++ClaimingShippedTable; }
		}

		if (!bEnumUnchanged || !bUnsafeRefused || bSafeRefused || Undeclared > 0
			|| NeedingBaseline == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] lever registry self-test (5) FAILED (baseline source law): enum shape "
				     "unchanged=%s; undeclared baseline refused=%s ('%s'); declared baseline "
				     "accepted=%s ('%s'); sweep saw %d writable lever(s), %d needing a baseline "
				     "(expected more than 0, or this sweep proved nothing), %d with NO declared "
				     "source (expected 0)."),
				bEnumUnchanged ? TEXT("yes") : TEXT("NO"),
				bUnsafeRefused ? TEXT("yes") : TEXT("NO"), *UnsafeWhy,
				bSafeRefused ? TEXT("NO") : TEXT("yes"), *SafeWhy,
				WritableSwept, NeedingBaseline, Undeclared);
			bOk = false;
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] lever registry self-test (5) passed: %d writable lever(s) swept, %d "
				     "compare against a baseline, 0 left the source undeclared, %d claim ShippedTable. "
				     "The enum still offers no live-read value, and the classifier refused an "
				     "undeclared baseline with: %s"),
				WritableSwept, NeedingBaseline, ClaimingShippedTable, *UnsafeWhy);

			// The honest gap, stated every boot rather than remembered. No shipped vanilla-defaults
			// table exists for engine r.* cvars, so every baseline-comparing lever uses the GUARDED
			// CapturedOnce path instead. That path refuses to read while FPM holds the cvar, which is
			// what stops the ratchet; it is not the same thing as a shipped table, and saying so is
			// the difference between a known gap and a forgotten one.
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] lever registry: %d baseline-comparing lever(s) use CapturedOnce and %d use "
				     "ShippedTable. ShippedTable is the source Law 3 prefers and NO shipped "
				     "vanilla-defaults table exists yet for engine r.* cvars, so the count above is "
				     "expected to be 0 today. The CapturedOnce guard (refuse a read while held) is "
				     "what carries the anti-ratchet duty until that table ships."),
				NeedingBaseline - ClaimingShippedTable, ClaimingShippedTable);
		}
	}

	return bOk;
}

// ------------------------------------------------------------------------------------------------
// IFPMFix
// ------------------------------------------------------------------------------------------------

void FFPMLeverRegistry::Arm()
{
	RegisterSelfTestLevers();
	bSelfTestPassed = SelfTest();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] lever registry armed (self-test %s). %d lever(s) registered - %d PRODUCTION, %d "
		     "self-test fixture(s) - and %d refused. Capability probing and the alias table build are "
		     "deferred to world load -- a cvar owned by a module that has not finished loading does "
		     "not exist yet here."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"), Levers.Num(),
		CountProduction(), CountSelfTestFixtures(), RefusedRegistrations.Num());
}

void FFPMLeverRegistry::OnWorldLoad(UWorld* World)
{
	if (!bSelfTestPassed)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry: skipping the probe pass -- the self-test failed at Arm, so "
			     "an Available/Absent verdict here would not be trustworthy."));
		return;
	}

	RefreshAliasTable();
	for (auto& Pair : Levers)
	{
		ProbeOne(Pair.Value);
	}

	int32 Available = 0;
	int32 Absent = 0;
	for (const auto& Pair : Levers)
	{
		if (Pair.Value.Availability == EFPMLeverAvailability::Available) { ++Available; }
		else if (Pair.Value.Availability == EFPMLeverAvailability::Absent) { ++Absent; }
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] lever registry: probe pass complete -- %d available, %d absent, of %d registered "
		     "(%d of those are self-test fixtures, not shipped levers)."),
		Available, Absent, Levers.Num(), CountSelfTestFixtures());
}

void FFPMLeverRegistry::Disarm()
{
	// Safety net: the self-test releases its own held-cvar probe before returning, so this should
	// find nothing live. Calling it anyway matches the house pattern of a Disarm that undoes
	// exactly what Arm's self-test could have left behind, rather than trusting the happy path.
	FPMCVarWriter::Get().ReleaseOwner(SelfTestOwnerName());

	Levers.Reset();
	AliasTable.Reset();
	RefusedRegistrations.Reset();
	bSelfTestPassed = false;
}

// ------------------------------------------------------------------------------------------------
// Coverage report.
// ------------------------------------------------------------------------------------------------

void FFPMLeverRegistry::ReportNow(FOutputDevice& Ar) const
{
	FPMScopedConsoleEcho Echo(&Ar);

	if (!bSelfTestPassed)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] lever registry: self-test FAILED at Arm (see the boot log for which check). "
			     "REFUSING to print a coverage table built on an unproven classifier."));
		return;
	}

	int32 Writable = 0;
	int32 ReadOnly = 0;
	int32 Available = 0;
	int32 Absent = 0;
	int32 Unprobed = 0;
	for (const auto& Pair : Levers)
	{
		const FFPMLeverDefinition& Def = Pair.Value;
		Def.bWritable ? ++Writable : ++ReadOnly;
		switch (Def.Availability)
		{
		case EFPMLeverAvailability::Available: ++Available; break;
		case EFPMLeverAvailability::Absent:    ++Absent;    break;
		default:                               ++Unprobed;  break;
		}
	}

	/*
	 * THE DISCLOSURE IS NOW COMPUTED, NOT WRITTEN DOWN (M2). The old line asserted "every lever below
	 * is a self-test fixture" in prose. That was true the day it was written and would have become
	 * false, silently, the day the first production lever registered. Counting is the only version of
	 * that sentence that cannot go stale.
	 */
	const int32 Fixtures = CountSelfTestFixtures();
	const int32 Production = CountProduction();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] lever registry -- %d lever(s) registered: %d PRODUCTION, %d self-test fixture(s). "
		     "%d writable, %d read-only, %d refused at registration. Probe state: %d available, %d "
		     "absent, %d not yet probed."),
		Levers.Num(), Production, Fixtures, Writable, ReadOnly, RefusedRegistrations.Num(),
		Available, Absent, Unprobed);

	UE_CLOG(Production == 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   NO PRODUCTION LEVER IS REGISTERED YET. This build ships the registry MECHANISM "
		     "only; every row below is a fixture (reserved name prefix %s). The production stage "
		     "tables (B1-B6/K1-K4) are a separate, not-yet-built item."),
		FPMLeverSelfTestPrefix());

	for (const auto& Pair : Levers)
	{
		const FFPMLeverDefinition& Def = Pair.Value;
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-11s %-34s %-9s %-10s policy=%-16s currency=%-16s baseline=%s"),
			Def.bIsSelfTestFixture ? TEXT("[fixture]") : TEXT("[production]"),
			*Def.Name.ToString(),
			Def.bWritable ? TEXT("writable") : TEXT("read-only"),
			LexToString(Def.Availability),
			LexToString(Def.Policy),
			*LexToString(Def.Currency),
			Def.bBaselineCaptured ? *Def.CapturedBaselineValue : TEXT("(none captured)"));
	}

	for (const FString& Refusal : RefusedRegistrations)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM]   REFUSED %s"), *Refusal);
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMLeverReportCmd(
	TEXT("FPM.Lever.Report"),
	TEXT("Lever registry: registration and probe coverage. Reads only -- writes no console "
	     "variable, no ini, nothing."),
	// Gate BEFORE the echo: ReportNow opens its own FPMScopedConsoleEcho, so the claim has to be made
	// out here. See FPMConsoleEcho.h for why one report per engine frame is the bound.
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMReportGate Gate(Ar, TEXT("FPM.Lever.Report"));
		if (Gate.IsRefused())
		{
			return;
		}

		FFPMLeverRegistry::Get().ReportNow(Ar);
	}));
