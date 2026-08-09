// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMCVarWriter.h"

#include "FicsitsPerformanceManager.h"

// GENERATED, beside this file. `FPMUserSettingTable::GDerivedUSBackedCVars` — the 66 vanilla cvars that
// really are user-setting-backed, read from each asset's own StrId/UseCVar rather than guessed from its
// name. Regenerate with 40-TOOLS/satisfactory/extract_user_settings.ps1.
#include "FPMUserSettingTable.g.h"

namespace
{
	/**
	 * ★ CLAUSE 6's LEGACY DENYLIST — RETAINED, NOT TRUSTED. The derived table beside it is the real one.
	 *
	 * ⚠ THIS LIST WAS BUILT BY GUESSING A CVAR NAME FROM AN ASSET NAME, AND IT GUESSED WRONG BOTH WAYS.
	 * It came from `20-SOURCES/satisfactory/FPM-US-DENYLIST-1.2.3.1.tsv`, whose columns are
	 * `us_asset / candidate_cvar / confidence`, and whose 242-of-272 `UNMAPPED` rate was read as "the
	 * danger we cannot yet spell". Re-derived from the assets themselves on 2026-08-09, that reading was
	 * wrong: a setting's cvar name is simply its `StrId`, present in every asset
	 * (FGUserSetting.h:183-189 — "manage and if needed create a cvar for this setting based on StrId").
	 * Reading StrId directly yields **66 cvar-backed vanilla settings and zero unmapped**. Of the old
	 * file's 272 rows, 184 name settings that drive no cvar at all, 4 are names truncated at a hyphen
	 * (`US_HierarchicalZ` for `US_HierarchicalZ-BufferOcclusion`), and 19 match no asset in the game
	 * (`US_34z`, `US_Jx`, `US_dT`, …). The "242 unmapped" was an artefact of how the file was made.
	 *
	 * ⚠ SO WHY IS IT STILL HERE? BECAUSE REMOVING AN ENTRY FROM A BAN LIST IS THE DIRECTION THAT CAN
	 * HURT. Every name below that the derived table lacks is either an asset with no cvar or a cvar no
	 * asset claims — both harmless to keep and, on this evidence, harmless to drop. But "on this
	 * evidence" is exactly the phrase that preceded the 242-unmapped mistake, and a false refusal costs
	 * a log line while a false permission costs a permanent change to the player's own settings. The two
	 * are not symmetric, so the union stands until something forces a choice.
	 *
	 * ⚠ AND THE DERIVED TABLE CAUGHT A LIVE HOLE THIS LIST HAD. `r.ContactShadows` — the cvar design
	 * §2.3.6 records as having actually leaked on 2026-08-02 — is ABSENT below, as are `r.Fog.Density`,
	 * `r.VolumetricCloud`, `r.HZBOcclusion`, `foliage.DitheredLOD` and ~50 more. Meanwhile `r.Gamma` is
	 * refused here while the real setting-backed cvar, `r.TonemapperGamma`, was not. It guarded a name
	 * the game does not use and left the one it does open.
	 *
	 * NEITHER TABLE IS THE PRIMARY. Both are VANILLA-ONLY snapshots, and mods register their own settings
	 * — confirmed 2026-08-09: the LightSettings mod's levers are mod-side SessionSettings assets that no
	 * export of the base game can contain. The runtime read of `GetAllUserSettingsMap()` is the primary
	 * and it sees those; these two are what remains when it cannot be reached.
	 *
	 * ⚠ Name prefixed for the UNITY BUILD (FPMFixContract.h:166-171).
	 */
	const TCHAR* const GFPMWriterUSLegacyGuesses[] =
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
	/*
	 * THE UNION OF BOTH TABLES, DERIVED FIRST because it is the one that answers correctly and carries
	 * the cvars the legacy list missed.
	 *
	 * ⚠ Stricmp, NEVER Strcmp, and this is not defensive habit — the assets themselves are inconsistently
	 * cased. `US_ScreenPercentage` declares its StrId as `r.screenpercentage` while the engine's own cvar
	 * is registered `r.ScreenPercentage`. A case-sensitive compare would walk straight past the guard on
	 * the exact cvar most likely to be written.
	 */
	for (const TCHAR* const Derived : FPMUserSettingTable::GDerivedUSBackedCVars)
	{
		if (FCString::Stricmp(CVarName, Derived) == 0) { return true; }
	}

	for (const TCHAR* const Guessed : GFPMWriterUSLegacyGuesses)
	{
		if (FCString::Stricmp(CVarName, Guessed) == 0) { return true; }
	}

	/*
	 * ⚠ FALSE IS NOT "SAFE", IT IS "NOT IN TWO VANILLA SNAPSHOTS", and the caller must not read it as
	 * more than that. A mod-registered setting is invisible to both tables by construction, so this
	 * answer is only as good as its inputs until P1.3's runtime map is consulted ahead of it.
	 */
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

void FPMCVarWriter::LogLedger(FOutputDevice* Ar) const
{
	// Both destinations, always. Ar reaches the console the operator is reading; the log is what
	// survives the session and what a support dump carries. Which one is "the" output depends on who
	// is asking, and on 2026-08-09 both of us needed it at once.
	auto Emit = [Ar](const FString& Line)
	{
		if (Ar) { Ar->Logf(TEXT("%s"), *Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	};

	Emit(FString::Printf(TEXT("---- cvar ledger: %d hold(s) ----"), Ledger.Num()));

	/*
	 * ★ EVERY ROW IS CHECKED AGAINST THE LIVE VARIABLE. Found 2026-08-09, mid-P1.5, by Ant.
	 *
	 * Until now this printed `H.Value` — what we ASKED to hold — and nothing else. So a hold that had
	 * been BEATEN by a higher-priority writer printed exactly the same line as one that was still in
	 * force. During the 0x07 proof protocol that is the entire question ("did our hold survive the
	 * options-menu apply?"), and the command answering it could not distinguish the two outcomes.
	 * A ledger cannot verify itself: it is a record of intent, and intent is not state.
	 *
	 * Now the live value is read back and compared. A divergence is the LOUDEST thing in the output,
	 * because it means either something outranked us — a real finding worth having — or our release
	 * path failed and the ledger is tracking a hold that no longer exists, which is residue.
	 */
	for (const FHold& H : Ledger)
	{
		IConsoleVariable* Live = IConsoleManager::Get().FindConsoleVariable(*H.CVar);
		const FString LiveValue = Live ? Live->GetString() : FString(TEXT("<GONE>"));
		const FString LiveSetBy = Live
			? FString(GetConsoleVariableSetByName(
				static_cast<EConsoleVariableFlags>(Live->GetFlags() & ECVF_SetByMask)))
			: FString(TEXT("-"));
		const bool bHolding = Live && LiveValue.Equals(H.Value);

		Emit(FString::Printf(
			TEXT("  %-44s = %-12s (was %-12s %s)  owner='%s' %s : %s"),
			*H.CVar, *H.Value, *H.PriorValue, GetConsoleVariableSetByName(H.PriorSetBy),
			*H.Owner.ToString(), H.Lease == EFPMLease::Module ? TEXT("module") : TEXT("world"),
			*H.Reason));
		Emit(FString::Printf(TEXT("      live: %-12s %-16s  %s"), *LiveValue, *LiveSetBy,
			bHolding
				? TEXT("HOLD IN FORCE")
				: TEXT("** OVERRIDDEN - our value is NOT what the game is using **")));
	}

	// An empty ledger is a RESULT, not an absence of output — it is what "FPM is holding nothing" looks
	// like, and it is the state an uninstall has to be able to demonstrate.
	if (Ledger.Num() == 0)
	{
		Emit(TEXT("  (holding nothing - the game is in its own state)"));
	}
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
static FAutoConsoleCommandWithOutputDevice GWriterLedgerCmd(
	TEXT("FPM.Changes"),
	TEXT("Print every console variable FPM is currently holding, with its prior value and prior SetBy."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
		{ FPMCVarWriter::Get().LogLedger(&Ar); }));

static FAutoConsoleCommandWithOutputDevice GWriterOffCmd(
	TEXT("FPM.Off"),
	TEXT("THE OFF SWITCH. Release every console variable FPM holds and leave the game in its own state."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		// Report what it DID, not that it ran. "FPM.Off" printing nothing is indistinguishable from
		// FPM.Off being broken -- the same defect FPM.Changes shipped with.
		FPMCVarWriter& W = FPMCVarWriter::Get();
		TArray<FString> Before;
		W.GetHeldCVars(Before);
		W.ReleaseAll(TEXT("FPM.Off from the console"));
		Ar.Logf(TEXT("FPM.Off: released %d hold(s)%s"), Before.Num(),
			Before.Num() ? *FString::Printf(TEXT(" - %s"), *FString::Join(Before, TEXT(", "))) : TEXT(" (nothing was held)"));
	}));

/*
 * ★ `FPM.Hold` / `FPM.Release` — WHAT MAKES P1.5 RUNNABLE AT ALL.
 *
 * Design R2 §9's P1.5 is "THE 0x07 PROOF BOOT", and it is the last Phase 1 increment: write at 0x07,
 * survive a scalability apply and a vanilla options-menu apply, confirm a console override still WINS,
 * then release and confirm BOTH the value and the SetBy came back. The recorded project law keeps
 * prescribing SetByCode until that boot lands, so this is blocking a law change and not a checkbox.
 *
 * ⚠ IT COULD NOT BE RUN. Discovered 2026-08-09 while writing the protocol out for Ant: nothing in the
 * shipped build makes the writer hold anything on demand. The boot self-test holds and releases inside
 * one frame, and no fix writes a cvar yet. So every instruction of the form "now hold a cvar and change
 * a setting" was unexecutable, and I had handed her exactly that. The gap was in the BUILD, not in the
 * protocol -- worth recording, because the protocol had been reviewed several times and nobody noticed
 * that the mod offered no way to perform step one.
 *
 * CLAUSE 6 STILL APPLIES. This routes through Hold(), so a US_*-backed cvar is refused here exactly as
 * it is everywhere else. That means this command covers P1.5's LEG A only. Leg B deliberately targets
 * `t.MaxFPS` -- a US_*-backed cvar -- to contest 0x08 SetByGameOverride, and crossing that boundary is
 * Ant's call, not a decision to bury inside a console command's implementation. See the note surfaced
 * with the build.
 */
static FAutoConsoleCommandWithArgsAndOutputDevice GWriterHoldCmd(
	TEXT("FPM.Hold"),
	TEXT("Hold a console variable through FPM's writer, at FPM's priority, until released. "
	     "Usage: FPM.Hold <cvar> <value>   (US_*-backed cvars are refused - clause 6)"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() != 2)
			{
				Ar.Logf(TEXT("usage: FPM.Hold <cvar> <value>"));
				return;
			}
			static const FName ProbeOwner(TEXT("console-probe"));
			const bool bOk = FPMCVarWriter::Get().Hold(ProbeOwner, *Args[0], *Args[1],
				TEXT("held by hand from the console (P1.5 proof protocol)"));
			// Hold() logs its own refusal reason; echo the VERDICT to the console so the operator is
			// not left reading an empty line and guessing whether it took.
			Ar.Logf(TEXT("FPM.Hold %s = %s : %s"), *Args[0], *Args[1],
				bOk ? TEXT("HELD") : TEXT("REFUSED - see the log line above for which clause"));
			if (bOk) { FPMCVarWriter::Get().LogLedger(&Ar); }
		}));

static FAutoConsoleCommandWithArgsAndOutputDevice GWriterReleaseCmd(
	TEXT("FPM.Release"),
	TEXT("Release one hold taken by FPM.Hold. Usage: FPM.Release <cvar>"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() != 1)
			{
				Ar.Logf(TEXT("usage: FPM.Release <cvar>"));
				return;
			}
			static const FName ProbeOwner(TEXT("console-probe"));
			const bool bOk = FPMCVarWriter::Get().Release(ProbeOwner, *Args[0]);
			Ar.Logf(TEXT("FPM.Release %s : %s"), *Args[0],
				bOk ? TEXT("RELEASED") : TEXT("we were not holding it (not an error)"));
		}));
