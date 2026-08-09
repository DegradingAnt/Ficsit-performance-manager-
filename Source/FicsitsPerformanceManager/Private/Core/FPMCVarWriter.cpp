// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMCVarWriter.h"

#include "FicsitsPerformanceManager.h"

namespace
{
	/**
	 * ★ CLAUSE 6's DENYLIST — the US_*-backed cvars, refused outright until P1.3.
	 *
	 * Derived from `20-SOURCES/satisfactory/FPM-US-DENYLIST-1.2.3.1.tsv`, read 2026-08-09: 272 US_*
	 * settings assets, of which only these carry a candidate cvar name. AMBIGUOUS rows listed several
	 * alternatives separated by `|`; **every alternative is listed here**, because refusing the wrong one
	 * of an ambiguous pair is the same as refusing none.
	 *
	 * ⚠ **242 OF THE 272 ARE `UNMAPPED` — THEY NAME NO CVAR AT ALL.** So this list cannot be complete and
	 * must never be read as "these are the dangerous ones". It is the part of the danger we can currently
	 * spell. That is exactly why clause 6 refuses the whole SUBSET rather than filtering by this table,
	 * and why P1.3 reads `UFGGameUserSettings::GetAllUserSettingsMap()` at RUNTIME instead of trusting a
	 * shipped snapshot. Written down here so a future reader does not mistake the table for the territory.
	 *
	 * ⚠ Name prefixed for the UNITY BUILD (FPMFixContract.h:166-171).
	 */
	const TCHAR* const GFPMWriterUSDenylist[] =
	{
		TEXT("CSS.Conveyor.MaxDrawDistance"),
		TEXT("FG.AlwaysShowVehiclePaths"),      TEXT("FG.ArachnophobiaMode"),
		TEXT("FG.ConveyorItemFrequency"),       TEXT("FG.DisableNarrativeMessages"),
		TEXT("FG.DismantleCratePlacementMode"), TEXT("FG.DisplayHologramClearance"),
		TEXT("FG.HoldZipline"),                 TEXT("FG.HologramRotationMode"),
		TEXT("FG.MergeDismantleCrates"),        TEXT("FG.PauseGameInPauseMenu"),
		TEXT("FG.SampleCopyCustomization"),     TEXT("FG.SampleCopySettings"),
		TEXT("FG.SelectCancelSwap"),            TEXT("FG.VehiclePathRenderDistance"),
		TEXT("TSR.AntiAliasing"),               TEXT("r.Mobile.AntiAliasing"),
		TEXT("r.AntiAliasingMethod"),
		TEXT("r.Bloom.ScreenPercentage"),       TEXT("r.ScreenPercentage"),
		TEXT("r.TSR.History.ScreenPercentage"),
		TEXT("r.DefaultFeature.MotionBlur"),    TEXT("r.FastVRam.MotionBlur"),
		TEXT("r.Gamma"),
		TEXT("r.HairStrands.DeepShadow.Resolution"),
		TEXT("r.HeterogeneousVolumes.Shadows.Resolution"),
		TEXT("r.HeterogeneousVolumes.Tessellation.BottomLevelGrid.Resolution"),
		TEXT("r.HeterogeneousVolumes.CompositeWithTranslucency.Refraction.Tr"),
		TEXT("r.Mobile.ScreenSpaceReflections"),
		TEXT("r.ShadowQuality"),
		TEXT("r.VSync"),                        TEXT("r.Vsync"),
		TEXT("t.MaxFPS"),
	};

	/** The one priority FPM writes at. Named once so no call site can quietly choose another. */
	constexpr EConsoleVariableFlags GFPMWriterPriority = ECVF_SetByPluginHighPriority;

	/**
	 * FPM's OWN cvar, existing purely so the self-test never touches a game cvar. Registering a real
	 * variable is what makes the test exercise the actual engine path rather than a mock of it.
	 */
	TAutoConsoleVariable<int32> CVarWriterProbe(
		TEXT("FPM.SelfTest.Probe"), 0,
		TEXT("FPM's own scratch variable. It steers nothing and is read by nothing; the cvar writer's "
		     "boot self-test writes and releases it so the release path is proven on every boot rather "
		     "than assumed. Safe to ignore."),
		ECVF_Default);
}

FPMCVarWriter& FPMCVarWriter::Get()
{
	static FPMCVarWriter Instance;
	return Instance;
}

const FName& FPMCVarWriter::Tag()
{
	// One tag for the whole mod. UnsetAllConsoleVariablesWithTag keys on it, so it is the OFF switch.
	static const FName Value(TEXT("FPM"));
	return Value;
}

IConsoleVariable* FPMCVarWriter::Vet(FName Owner, const TCHAR* CVarName) const
{
	// CLAUSE 1 — existence. First because it is cheapest AND because it is the one whose silent failure
	// already cost this project months of misattributed measurements.
	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': no such console variable in this build. "
			     "A write to a cvar that does not exist is a silent no-op, and every measurement "
			     "attributed to it would be attributed wrongly. Check the spelling against the running "
			     "game, not against a doc."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	// CLAUSE 5 — banned route. Scalability GROUPS leak through Scalability::SaveState with no gate.
	// Drive the members the group expands to; never the group.
	if (FCString::Strnicmp(CVarName, TEXT("sg."), 3) == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': sg.* scalability groups leak through "
			     "Scalability::SaveState with no gate. Expand the group and drive its member cvars."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	// CLAUSE 6 — the US_*-backed set, refused ENTIRELY until P1.3's SaveSettings interceptor exists.
	if (IsUserSettingBacked(CVarName))
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer REFUSED '%s' for owner '%s': it is backed by a US_* game user setting. "
			     "FGGameUserSettings serialises every entry on every save with NO dirty gate, so a value "
			     "we hold at save time would become the player's PERMANENT setting and survive uninstall. "
			     "This is refused until P1.3 ships the SaveSettings interceptor - not tunable, not a bug."),
			CVarName, *Owner.ToString());
		return nullptr;
	}

	return Var;
}

bool FPMCVarWriter::IsUserSettingBacked(const TCHAR* CVarName)
{
	for (const TCHAR* const Denied : GFPMWriterUSDenylist)
	{
		if (FCString::Stricmp(CVarName, Denied) == 0) { return true; }
	}
	return false;
}

void FPMCVarWriter::GetHeldCVars(TArray<FString>& Out) const
{
	Out.Reset();
	Out.Reserve(Ledger.Num());
	for (const FHold& H : Ledger) { Out.Add(H.CVar); }
}

bool FPMCVarWriter::Hold(FName Owner, const TCHAR* CVarName, const TCHAR* Value, const TCHAR* Reason,
                         EFPMLease Lease)
{
	IConsoleVariable* Var = Vet(Owner, CVarName);
	if (!Var) { return false; }

	/*
	 * CLAUSE 4 — ownership, in OBSERVE MODE. Deliberately staged: a day-one hard error here bricks a
	 * session over a bookkeeping mistake, which is the lesson the old mod's S4 taught. It warns for one
	 * release and enforces in the next.
	 *
	 * ⚠ THE SAME OWNER RE-REGISTERING IS NOT A COLLISION. Owner identity is a stable NAME, so the next
	 * world's instance of a world-scoped owner RECLAIMS its own hold. Keying this on an instance pointer
	 * would make every world transition look like a conflict.
	 */
	for (const FHold& Existing : Ledger)
	{
		if (!Existing.CVar.Equals(CVarName, ESearchCase::IgnoreCase)) { continue; }
		if (Existing.Owner == Owner) { break; }   // reclaim, not collision

		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] writer: '%s' is already held by '%s' and '%s' is writing it too. OBSERVE MODE - "
			     "the write proceeds and this becomes a refusal in the next release. Two owners on one "
			     "cvar means the release order decides the final value, which is not a thing to leave to "
			     "chance."),
			CVarName, *Existing.Owner.ToString(), *Owner.ToString());
		break;
	}

	/*
	 * ★ THE PRIOR VALUE IS THE PLAYER'S, NOT OUR LAST ONE — review finding, 2026-08-09.
	 *
	 * Reading the cvar here is only correct the FIRST time we hold it. On a RE-HOLD (a reclaim, or the
	 * governor moving a lever to a new value) the current value is the one WE wrote, so recording it
	 * would make `FPM.Changes` answer "what has this mod changed?" with a value the mod itself set. The
	 * engine's history still releases correctly — this was never a residue bug — but the ledger is the
	 * thing a support dump is read from, and a ledger that misreports the baseline is worse than none.
	 *
	 * So: if we already hold this cvar for this owner, KEEP the prior we captured the first time.
	 */
	FString PriorValue = Var->GetString();
	EConsoleVariableFlags PriorSetBy =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	if (const FHold* Previous = Ledger.FindByPredicate([&](const FHold& H)
			{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); }))
	{
		PriorValue = Previous->PriorValue;
		PriorSetBy = Previous->PriorSetBy;
	}

	// THE WRITE. Tagged, so the engine-native release can find it; at 0x07, so the console still wins.
	Var->Set(Value, GFPMWriterPriority, Tag());

	// Drop any stale entry for this cvar+owner so the ledger cannot grow duplicates across reclaims.
	Ledger.RemoveAll([&](const FHold& H)
		{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });

	FHold Entry;
	Entry.CVar       = CVarName;
	Entry.Value      = Value;
	Entry.PriorValue = PriorValue;
	Entry.PriorSetBy = PriorSetBy;
	Entry.Owner      = Owner;
	Entry.Reason     = Reason;
	Entry.Lease      = Lease;
	Ledger.Add(MoveTemp(Entry));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer HOLD %s = %s (was %s, %s) owner='%s' lease=%s : %s"),
		CVarName, Value, *PriorValue, GetConsoleVariableSetByName(PriorSetBy), *Owner.ToString(),
		Lease == EFPMLease::Module ? TEXT("module") : TEXT("world"), Reason);
	return true;
}

bool FPMCVarWriter::Release(FName Owner, const TCHAR* CVarName)
{
	const int32 Index = Ledger.IndexOfByPredicate([&](const FHold& H)
		{ return H.Owner == Owner && H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });

	if (Index == INDEX_NONE)
	{
		// Said out loud rather than returning quietly. "We released it" and "we never held it" are
		// different facts and only one of them means the revert worked.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] writer: nothing to release - '%s' is not held by '%s'."),
			CVarName, *Owner.ToString());
		return false;
	}

	if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName))
	{
		// ⚠ Unset, NOT a lower-priority Set. 0x07 is an ARRAY-typed priority: a Set would APPEND to the
		// history and leave our entry in it forever, which looks exactly like a working revert.
		Var->Unset(GFPMWriterPriority, Tag());
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer RELEASE %s (owner '%s')"), CVarName, *Owner.ToString());
	Ledger.RemoveAt(Index);
	return true;
}

int32 FPMCVarWriter::ReleaseOwner(FName Owner)
{
	TArray<FString> Mine;
	for (const FHold& H : Ledger)
	{
		if (H.Owner == Owner) { Mine.Add(H.CVar); }
	}
	for (const FString& CVar : Mine) { Release(Owner, *CVar); }

	UE_CLOG(Mine.Num() > 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer: released %d hold(s) for owner '%s'."), Mine.Num(), *Owner.ToString());
	return Mine.Num();
}

void FPMCVarWriter::ReleaseAll(const TCHAR* Reason)
{
	const int32 Count = Ledger.Num();

	// ONE ENGINE CALL. This is the OFF switch's mechanism (IConsoleManager.h:1243) and it is why the
	// switch cannot half-work: there is no per-cvar loop here to get partway through and fail.
	IConsoleManager::Get().UnsetAllConsoleVariablesWithTag(Tag(), GFPMWriterPriority);
	Ledger.Reset();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] writer RELEASE ALL - %d hold(s) dropped via the engine's tagged history (%s). "
		     "Nothing was captured, so nothing can be restored wrongly."),
		Count, Reason);
}

bool FPMCVarWriter::IsHeld(const TCHAR* CVarName) const
{
	return Ledger.ContainsByPredicate([&](const FHold& H)
		{ return H.CVar.Equals(CVarName, ESearchCase::IgnoreCase); });
}

void FPMCVarWriter::LogLedger() const
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] ---- cvar ledger: %d hold(s) ----"), Ledger.Num());

	for (const FHold& H : Ledger)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-44s = %-12s (was %-12s %s)  owner='%s' %s : %s"),
			*H.CVar, *H.Value, *H.PriorValue, GetConsoleVariableSetByName(H.PriorSetBy),
			*H.Owner.ToString(), H.Lease == EFPMLease::Module ? TEXT("module") : TEXT("world"),
			*H.Reason);
	}

	// An empty ledger is a RESULT, not an absence of output — it is what "FPM is holding nothing" looks
	// like, and it is the state an uninstall has to be able to demonstrate.
	UE_CLOG(Ledger.Num() == 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   (holding nothing - the game is in its own state)"));
}

bool FPMCVarWriter::SelfTest()
{
	static const FName Owner(TEXT("writer-selftest"));
	const TCHAR* const Probe = TEXT("FPM.SelfTest.Probe");

	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Probe);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] writer self-test CANNOT RUN: '%s' is missing. The writer is unverified this boot."),
			Probe);
		return false;
	}

	const FString Before = Var->GetString();
	const EConsoleVariableFlags SetByBefore =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	if (!Hold(Owner, Probe, TEXT("4242"), TEXT("boot self-test of the write path")))
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] writer self-test FAILED: the hold was refused on FPM's own probe cvar."));
		return false;
	}

	const FString Held = Var->GetString();
	Release(Owner, Probe);

	const FString After = Var->GetString();
	const EConsoleVariableFlags SetByAfter =
		static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

	// BOTH halves are checked. A release that restores the VALUE but leaves our SetBy on the variable has
	// not let go — the next writer at a lower priority would still be locked out, and the residue would
	// be invisible to anyone reading only the value.
	const bool bValueOk = (Held == TEXT("4242")) && (After == Before);
	const bool bSetByOk = (SetByAfter == SetByBefore);

	if (bValueOk && bSetByOk)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] writer self-test PASSED: %s %s -> 4242 -> %s, SetBy %s -> %s. Write, hold and "
			     "engine-native release all work on this build."),
			Probe, *Before, *After,
			GetConsoleVariableSetByName(SetByBefore), GetConsoleVariableSetByName(SetByAfter));
		return true;
	}

	UE_LOG(LogFicsitsPerformanceManager, Error,
		TEXT("[FPM] writer self-test FAILED: value %s -> %s -> %s (expected back to '%s'), "
		     "SetBy %s -> %s (expected back to %s). ⚠ THE RELEASE PATH IS NOT WORKING ON THIS BUILD - "
		     "treat every hold as residue until this is understood."),
		*Before, *Held, *After, *Before,
		GetConsoleVariableSetByName(SetByBefore), GetConsoleVariableSetByName(SetByAfter),
		GetConsoleVariableSetByName(SetByBefore));
	return false;
}

/*
 * `FPM.Changes` (§7.15) — what FPM is currently holding, and what it was before.
 *
 * This is the question a player or a support dump actually asks, and until now nothing could answer it:
 * "what has this mod changed on my machine?" The honest answer is a list with prior values beside it.
 */
static FAutoConsoleCommand GWriterLedgerCmd(
	TEXT("FPM.Changes"),
	TEXT("Print every console variable FPM is currently holding, with its prior value and prior SetBy."),
	FConsoleCommandDelegate::CreateLambda([]() { FPMCVarWriter::Get().LogLedger(); }));

static FAutoConsoleCommand GWriterOffCmd(
	TEXT("FPM.Off"),
	TEXT("THE OFF SWITCH. Release every console variable FPM holds and leave the game in its own state."),
	FConsoleCommandDelegate::CreateLambda([]()
		{ FPMCVarWriter::Get().ReleaseAll(TEXT("FPM.Off from the console")); }));
