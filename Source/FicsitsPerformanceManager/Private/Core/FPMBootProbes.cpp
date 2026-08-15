// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMBootProbes.h"

#include "FicsitsPerformanceManager.h"

#include "FGCharacterPlayer.h"
#include "FGTimeSubsystem.h"

#include "Equipment/FGEquipment.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"

namespace
{
	/** Same duplicated-emit discipline `FPMCVarProbe.cpp`'s `FPMProbeEmit` uses: Ar reaches the console
	 *  Ant is looking at, UE_LOG reaches the file an agent reads afterwards. Kept local rather than
	 *  shared because it is four lines and a shared header for four lines is its own kind of drift risk. */
	void FPMBootProbeEmit(FOutputDevice* Ar, const FString& Line)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	}

	/** `mEquipmentSlot`'s enumerators, spelled out by hand rather than through `StaticEnum<>` — this
	 *  file has no existing precedent for `StaticEnum` anywhere in the tree, and the seven names below
	 *  are read directly off `FGEquipment.h:38-47`, not guessed. */
	const TCHAR* FPMBootProbeEquipmentSlotName(EEquipmentSlot Slot)
	{
		switch (Slot)
		{
		case EEquipmentSlot::ES_NONE: return TEXT("ES_NONE");
		case EEquipmentSlot::ES_ARMS: return TEXT("ES_ARMS");
		case EEquipmentSlot::ES_BACK: return TEXT("ES_BACK");
		case EEquipmentSlot::ES_LEGS: return TEXT("ES_LEGS");
		case EEquipmentSlot::ES_HEAD: return TEXT("ES_HEAD");
		case EEquipmentSlot::ES_BODY: return TEXT("ES_BODY");
		default: return TEXT("ES_MAX/unknown");
		}
	}

	/** One row of `tools/sg_expansions.tsv`: group, level, the cvar it expands to, the banked value, and
	 *  where the table says that value came from (engine default vs game override). */
	struct FPMBootProbeSgRow
	{
		FString Group;
		FString Level;
		FString CVar;
		FString ExpectedValue;
		FString Source;
	};

	/**
	 * Reads `tools/sg_expansions.tsv` from THIS plugin's own directory, resolved through
	 * `IPluginManager` the same way `FPMBoxCache::GetCacheFilePath` and `FPMResidueSentinel` already do
	 * — never a hand-typed absolute path, which would be the first thing to go stale on another
	 * machine's checkout.
	 *
	 * Returns false only when the file could not be read at all (plugin not found by IPluginManager, or
	 * the file is missing). A malformed LINE inside an otherwise-readable file is skipped and counted
	 * in OutSkippedLines, not treated as a reason to abort the whole read.
	 */
	bool FPMBootProbeLoadSgTable(TArray<FPMBootProbeSgRow>& OutRows, FString& OutPath, int32& OutSkippedLines)
	{
		OutSkippedLines = 0;

		const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager"));
		if (!Self.IsValid()) { OutPath = TEXT("(plugin not found by IPluginManager)"); return false; }

		OutPath = FPaths::ConvertRelativePathToFull(Self->GetBaseDir()) / TEXT("tools") / TEXT("sg_expansions.tsv");

		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *OutPath)) { return false; }

		TArray<FString> Lines;
		Raw.ParseIntoArrayLines(Lines, /*InCullEmpty*/ true);

		// Row 0 is the header (group / level / cvar / value / source) - start at 1.
		for (int32 i = 1; i < Lines.Num(); ++i)
		{
			TArray<FString> Cols;
			Lines[i].ParseIntoArray(Cols, TEXT("\t"), /*InCullEmpty*/ false);
			if (Cols.Num() != 5) { ++OutSkippedLines; continue; }
			OutRows.Add(FPMBootProbeSgRow{ Cols[0], Cols[1], Cols[2], Cols[3], Cols[4] });
		}
		return true;
	}

	/** Strips a trailing " ; comment" the tsv's own VALUE column sometimes carries inline (e.g.
	 *  "0 ; Low quality", transcribed straight off DefaultScalability.ini), then trims whitespace. */
	FString FPMBootProbeStripValueComment(const FString& Value)
	{
		FString Head, Tail;
		if (Value.Split(TEXT(";"), &Head, &Tail)) { return Head.TrimStartAndEnd(); }
		return Value.TrimStartAndEnd();
	}

	/** Numeric-tolerant compare: parses both sides as a double when both look numeric (so "1" and
	 *  "1.000000" agree), falls back to a case-insensitive string compare otherwise. */
	bool FPMBootProbeValuesMatch(const FString& Expected, const FString& Live)
	{
		const FString E = FPMBootProbeStripValueComment(Expected);
		const FString L = FPMBootProbeStripValueComment(Live);
		if (E.IsNumeric() && L.IsNumeric())
		{
			return FMath::IsNearlyEqual(FCString::Atod(*E), FCString::Atod(*L), 1e-3);
		}
		return E.Equals(L, ESearchCase::IgnoreCase);
	}
}

void FPMBootProbes::ReportTimeOfDay(UWorld* World, FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.TimeOfDay (B9: does FGTimeOfDaySubsystem expose a "
	                          "non-cheat pin API?) ----"));

	/*
	 * ★ THE EXISTENCE HALF IS ALREADY SETTLED FROM SOURCE, STATED HERE SO THE LOG CARRIES THE ANSWER
	 * BESIDE THE READ, NOT ONLY IN A DESIGN DOC. `SetDaySeconds(float)` (FGTimeSubsystem.h:48) and
	 * `SetTimeSpeedMultiplier(float)` (:128) are both plain `public:` C++ members, never
	 * `UFUNCTION(exec, CheatBoard, ...)` the way UFGCheatManager's day/night controls are. Nothing
	 * below calls either — this command reads only.
	 */
	FPMBootProbeEmit(Ar, TEXT("  source read: AFGTimeOfDaySubsystem::SetDaySeconds / SetTimeSpeedMultiplier "
	                          "are public, non-cheat C++ members (FGTimeSubsystem.h:48,128) - a pin API "
	                          "EXISTS. Not called here; this command is read-only by design."));

	if (World == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no UWorld - run this from an active session, not the main menu. **"));
		return;
	}

	AFGTimeOfDaySubsystem* TimeOfDay = AFGTimeOfDaySubsystem::Get(World);
	if (TimeOfDay == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** AFGTimeOfDaySubsystem::Get() returned null on a real world - the "
		                          "subsystem is not up yet (too early in load) or this build moved it. "
		                          "Coverage: this probe can only report what it can reach; a null here is "
		                          "a finding, not a silent skip. Re-run once fully loaded in. **"));
		return;
	}

	FPMBootProbeEmit(Ar, FString::Printf(
		TEXT("  reachable: day %d, %.1fh (%.0fs into the day), day length %.1fmin, night length %.1fmin."),
		TimeOfDay->GetPassedDays(), TimeOfDay->GetTimeOfDayHours(), TimeOfDay->GetDaySeconds(),
		TimeOfDay->GetDayLength(), TimeOfDay->GetNightLength()));
	FPMBootProbeEmit(Ar, TEXT("  => B9 answered: subsystem reachable, pin API exists and is public. "
	                          "Verifying the PIN itself (call SetDaySeconds, confirm the clock holds) is "
	                          "follow-up work for whoever builds the lever, not this read-only probe."));
}

void FPMBootProbes::ReportSockets(UWorld* World, FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.Sockets (B11: which sockets does the vanilla player skeleton "
	                          "expose per forearm/hand, both sides?) ----"));

	if (World == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no UWorld - run this from an active session, not the main menu. **"));
		return;
	}

	APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	if (Character == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no local AFGCharacterPlayer pawn - not spawned in yet (main menu, "
		                          "loading screen, mid-respawn). Coverage: this probe needs a live "
		                          "character to read a skeleton off of; that is the whole precondition, "
		                          "stated rather than a silent empty result. Re-run once spawned in. **"));
		return;
	}

	/*
	 * ★ THE TARGETED CHECK, NOT JUST A DUMP. FPMWristItemBase.h:118,121 already ships a guess -
	 * "hand_lSocket" / "hand_rSocket" - and its own comment says B11 has never measured it against the
	 * real skeleton. So the useful answer is PASS/FAIL on the name the shipped code already depends on,
	 * on both meshes a wrist item could plausibly attach to - printed beside the full list so a wrong
	 * guess can be corrected from this one command instead of costing a second boot.
	 */
	const FName CandidateLeft(TEXT("hand_lSocket"));
	const FName CandidateRight(TEXT("hand_rSocket"));

	auto ReportMesh = [Ar](const TCHAR* Label, USkeletalMeshComponent* Mesh)
	{
		if (Mesh == nullptr)
		{
			FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %s: not present on this character."), Label));
			return;
		}

		const bool bLeft = Mesh->DoesSocketExist(FName(TEXT("hand_lSocket")));
		const bool bRight = Mesh->DoesSocketExist(FName(TEXT("hand_rSocket")));
		FPMBootProbeEmit(Ar, FString::Printf(
			TEXT("  %s: FPMWristItemBase's guess - hand_lSocket %s, hand_rSocket %s."),
			Label, bLeft ? TEXT("FOUND") : TEXT("NOT FOUND"), bRight ? TEXT("FOUND") : TEXT("NOT FOUND")));

		TArray<FName> All = Mesh->GetAllSocketNames();
		All.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %s: %d socket(s) total - %s"),
			Label, All.Num(),
			All.Num() > 0 ? *FString::JoinBy(All, TEXT(", "), [](const FName& N) { return N.ToString(); })
			              : TEXT("(none - this mesh has no sockets, which is itself a finding)")));
	};

	ReportMesh(TEXT("first-person (GetMesh1P)"), Character->GetMesh1P());
	ReportMesh(TEXT("third-person (GetMesh3P)"), Character->GetMesh3P());

	FPMBootProbeEmit(Ar, TEXT("  => B11 answered for this skeleton build: read the FOUND/NOT FOUND lines "
	                          "above. A NOT FOUND on either candidate means FPMWristItemBase.h's "
	                          "mSocketLeft/mSocketRight defaults need updating to a real name from the "
	                          "full list beside it - same boot, no second session needed."));
}

void FPMBootProbes::ReportEquipSlot(UWorld* World, FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.EquipSlot (B10: does the jetpack/hoverpack constructor set "
	                          "ES_BACK?) ----"));

	FPMBootProbeEmit(Ar, TEXT("  source read: AFGJetPack and AFGHoverPack both run mEquipmentSlot = "
	                          "EEquipmentSlot::ES_BACK in their constructor (FGJetPack.cpp:14, "
	                          "FGHoverPack.cpp:39) - ES_BACK occupied EXISTS from source. This probe "
	                          "confirms it live, on whatever the local character actually has equipped, "
	                          "rather than repeat the static read."));

	if (World == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no UWorld - run this from an active session, not the main menu. **"));
		return;
	}

	APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	if (Character == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no local AFGCharacterPlayer pawn - not spawned in yet (main menu, "
		                          "loading screen, mid-respawn). Coverage: this probe needs a live "
		                          "character to read an equipment slot off of; that is the whole "
		                          "precondition, stated rather than a silent empty result. **"));
		return;
	}

	AFGEquipment* BackItem = Character->GetEquipmentInSlot(EEquipmentSlot::ES_BACK);
	if (BackItem == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ES_BACK: nothing equipped right now. Coverage: this probe can only "
		                          "read what is actually worn - equip a jetpack or hoverpack and re-run "
		                          "to get a positive answer. **"));
		return;
	}

	FPMBootProbeEmit(Ar, FString::Printf(
		TEXT("  ES_BACK: occupied by %s (its own mEquipmentSlot reads %s)."),
		*BackItem->GetClass()->GetName(), FPMBootProbeEquipmentSlotName(BackItem->mEquipmentSlot)));
	FPMBootProbeEmit(Ar, TEXT("  => B10 answered live: see the class name above. Jetpack or HoverPack "
	                          "confirms the source read; anything else equipped in ES_BACK (SuitBase, "
	                          "Parachute, JumpingStilts all also set ES_BACK) is a DIFFERENT finding - "
	                          "ES_BACK is not exclusive to the jet/hover pack pair."));
}

void FPMBootProbes::ReportScalabilityGroups(FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.ScalabilityGroups (B4: does the live game expose 15 sg.* "
	                          "scalability groups or 10, and does each cvar the banked table expects "
	                          "still match?) ----"));

	TArray<FPMBootProbeSgRow> Rows;
	FString TsvPath;
	int32 SkippedLines = 0;
	if (!FPMBootProbeLoadSgTable(Rows, TsvPath, SkippedLines))
	{
		FPMBootProbeEmit(Ar, FString::Printf(
			TEXT("  ** could not read tools/sg_expansions.tsv (looked at %s). Coverage: 0/15 groups "
			     "checked, 0 rows diffed - this is a finding about the PROBE's environment (plugin not "
			     "found, or the file is missing from this checkout), not about the game. **"),
			*TsvPath));
		return;
	}
	if (SkippedLines > 0)
	{
		FPMBootProbeEmit(Ar, FString::Printf(
			TEXT("  %d malformed line(s) in the tsv were skipped (not exactly 5 tab-separated columns)."),
			SkippedLines));
	}

	// 1. THE GROUP COUNT - settles "15 vs 10" on its own: every group name the banked table carries,
	// checked for a live sg.<Name> selector cvar.
	TSet<FString> GroupNameSet;
	for (const FPMBootProbeSgRow& Row : Rows) { GroupNameSet.Add(Row.Group); }
	TArray<FString> GroupNames = GroupNameSet.Array();
	GroupNames.Sort();

	TMap<FString, IConsoleVariable*> LiveGroupCVars;
	for (const FString& Group : GroupNames)
	{
		IConsoleVariable* GroupCVar =
			IConsoleManager::Get().FindConsoleVariable(*FString::Printf(TEXT("sg.%s"), *Group));
		if (GroupCVar != nullptr) { LiveGroupCVars.Add(Group, GroupCVar); }

		FPMBootProbeEmit(Ar, FString::Printf(TEXT("  sg.%-28s %s%s"), *Group,
			GroupCVar != nullptr ? TEXT("FOUND") : TEXT("NOT FOUND"),
			GroupCVar != nullptr
				? *FString::Printf(TEXT(" (current level %d)"), GroupCVar->GetInt())
				: TEXT("")));
	}
	FPMBootProbeEmit(Ar, FString::Printf(
		TEXT("  => group count: %d/%d banked groups exist live on this build. The live game exposes %d "
		     "sg.* scalability groups (banked table carries 15)."),
		LiveGroupCVars.Num(), GroupNames.Num(), LiveGroupCVars.Num()));

	// 2. THE ROW-BY-ROW DIFF, against whichever level each live group is CURRENTLY AT. Only rows for a
	// live group at its current level are checkable; everything else is counted, not silently dropped.
	int32 Checked = 0, Passed = 0, Mismatched = 0, MissingCVar = 0;
	int32 SkippedNotCurrentLevel = 0, SkippedGroupAbsent = 0;
	for (const FPMBootProbeSgRow& Row : Rows)
	{
		IConsoleVariable** GroupCVarPtr = LiveGroupCVars.Find(Row.Group);
		if (GroupCVarPtr == nullptr) { ++SkippedGroupAbsent; continue; }

		const int32 CurrentLevel = (*GroupCVarPtr)->GetInt();
		if (!Row.Level.IsNumeric() || FCString::Atoi(*Row.Level) != CurrentLevel)
		{
			// Covers the "Cine" rows too - a non-numeric level cannot equal a live int level, so this
			// is the honest way those rows fall out rather than a special case for the string "Cine".
			++SkippedNotCurrentLevel;
			continue;
		}

		++Checked;
		IConsoleVariable* LeafCVar = IConsoleManager::Get().FindConsoleVariable(*Row.CVar);
		if (LeafCVar == nullptr)
		{
			++MissingCVar;
			FPMBootProbeEmit(Ar, FString::Printf(
				TEXT("  DIFF  %s @ %s : %s expected %s - MISSING, no such live console variable"),
				*Row.Group, *Row.Level, *Row.CVar, *Row.ExpectedValue));
			continue;
		}

		if (FPMBootProbeValuesMatch(Row.ExpectedValue, LeafCVar->GetString()))
		{
			++Passed;
		}
		else
		{
			++Mismatched;
			FPMBootProbeEmit(Ar, FString::Printf(
				TEXT("  DIFF  %s @ %s : %s expected %s, live reads %s"),
				*Row.Group, *Row.Level, *Row.CVar, *Row.ExpectedValue, *LeafCVar->GetString()));
		}
	}

	FPMBootProbeEmit(Ar, FString::Printf(
		TEXT("  => row diff: %d row(s) checked at the group's CURRENT level (%d pass, %d mismatch, %d "
		     "missing cvar - only mismatches and missing cvars are printed above; matching rows are "
		     "counted, not printed). %d row(s) belong to a level the group is not currently at (skipped, "
		     "not a finding) and %d row(s) belong to a group absent from this build (skipped, already "
		     "counted in the group total above)."),
		Checked, Passed, Mismatched, MissingCVar, SkippedNotCurrentLevel, SkippedGroupAbsent));

	if (Checked > 0 && Mismatched == 0 && MissingCVar == 0)
	{
		FPMBootProbeEmit(Ar, TEXT("  => B4 answered: the banked table stands - every checkable row at "
		                          "the current level matched."));
	}
}

void FPMBootProbes::ReportBuildableTick(FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.BuildableTick (B6: is EnableBuildableTick's mechanism "
	                          "reachable without the cheat manager?) ----"));

	FPMBootProbeEmit(Ar, TEXT("  source read: UFGCheatManager::EnableBuildableTick is "
	                          "UFUNCTION(exec, CheatBoard, category=\"Factory\") (FGCheatManager.h:797-798) "
	                          "- the only KNOWN access path runs through the cheat manager's exec surface. "
	                          "Its .cpp body ships as a stripped stub in this header package (no "
	                          "implementation), so the mechanism BEHIND it cannot be read statically; this "
	                          "probe searches for an independent console-object route instead."));

	static const TCHAR* Candidates[] = { TEXT("BuildableTick"), TEXT("FactoryTick"), TEXT("EffectUpdate") };

	TArray<FString> FoundNames;
	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
		FConsoleObjectVisitor::CreateLambda([&FoundNames](const TCHAR* Name, IConsoleObject* /*Obj*/)
		{
			const FString NameStr(Name);
			for (const TCHAR* Candidate : Candidates)
			{
				if (NameStr.Contains(Candidate, ESearchCase::IgnoreCase))
				{
					FoundNames.Add(NameStr);
					break;
				}
			}
		}),
		TEXT("")); // empty prefix - every registered console object, since the search is by substring

	if (FoundNames.Num() == 0)
	{
		FPMBootProbeEmit(Ar, TEXT("  no console variable or command whose name contains \"BuildableTick\", "
		                          "\"FactoryTick\" or \"EffectUpdate\" is registered on this build. The "
		                          "console-object route is UNREACHABLE by this search."));
	}
	else
	{
		FoundNames.Sort();
		FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %d candidate console object(s) found:"), FoundNames.Num()));
		for (const FString& Name : FoundNames)
		{
			FPMBootProbeEmit(Ar, FString::Printf(TEXT("    %s"), *Name));
		}
	}

	FPMBootProbeEmit(Ar, TEXT("  ** coverage: this probe does not attempt to read "
	                          "APlayerController::CheatManager. That member lives in the Engine module, "
	                          "not in this project's own FactoryGame header package, and this project's "
	                          "rule is never to write a signature from memory. So the CheatManager route "
	                          "is UNVERIFIED BY THIS PROBE - not proven reachable, not proven unreachable - "
	                          "only the console-object route above is answered. **"));

	FPMBootProbeEmit(Ar, TEXT("  => B6 answered for the console-object half: see the FOUND/NOT FOUND "
	                          "result above. If nothing was found, the design's own fallback holds - the "
	                          "lever is mEffectUpdateInterval only (source-confirmed private "
	                          "EditDefaultsOnly float, FGBuildableFactory.h:678-679, Category \"Anim\")."));
}

void FPMBootProbes::ReportGIMethod(FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.GIMethod (B20: does r.DynamicGlobalIlluminationMethod read 0 "
	                          "while Lumen is visibly ON, and which layer set it?) ----"));

	auto ReportOne = [Ar](const TCHAR* CVarName)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
		if (Var == nullptr)
		{
			FPMBootProbeEmit(Ar, FString::Printf(
				TEXT("  %s is not a registered console variable on this build."), CVarName));
			return;
		}
		const EConsoleVariableFlags SetBy = static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);
		FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %s = %s, set by %s."),
			CVarName, *Var->GetString(), GetConsoleVariableSetByName(SetBy)));
	};

	ReportOne(TEXT("r.DynamicGlobalIlluminationMethod"));
	// Corroborating read: this is the cvar the banked sg.* table's GlobalIlluminationQuality group
	// actually ties to Lumen (tools/sg_expansions.tsv: levels 2/3/Cine set it to 1, levels 0/1 set it
	// to 0), and Ant's "Lumen is visibly ON" observation is about THIS switch as much as the method
	// selector above.
	ReportOne(TEXT("r.Lumen.DiffuseIndirect.Allow"));

	FPMBootProbeEmit(Ar, TEXT("  => B20 answered: read both lines above beside whatever Ant is looking at "
	                          "on screen at the moment she runs this. A DynamicGlobalIlluminationMethod of "
	                          "0 next to a Lumen.DiffuseIndirect.Allow of 1 is exactly the 'stale or "
	                          "wrong-layer read' the design's default already assumes (design section 9.1)."));
}

/*
 * `FPM.Probe.TimeOfDay` and `FPM.Probe.Sockets` — plain read-only console commands, same registration
 * shape `FPM.Crates.Report` uses (`FAutoConsoleCommandWithWorldArgsAndOutputDevice`) so the delegate is
 * handed a `UWorld*` without either probe having to find one itself.
 */
static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMDiagTimeOfDayCmd(
	TEXT("FPM.Probe.TimeOfDay"),
	TEXT("B9: report whether AFGTimeOfDaySubsystem is reachable and confirm its pin API (SetDaySeconds / "
	     "SetTimeSpeedMultiplier) is public and non-cheat. Read-only - calls no setter."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMBootProbes::ReportTimeOfDay(World, &Ar);
		}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMDiagSocketsCmd(
	TEXT("FPM.Probe.Sockets"),
	TEXT("B11: enumerate socket names on the local player's first- and third-person mesh, and check "
	     "FPMWristItemBase's hand_lSocket/hand_rSocket guess against both. Requires a spawned character."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMBootProbes::ReportSockets(World, &Ar);
		}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMDiagEquipSlotCmd(
	TEXT("FPM.Probe.EquipSlot"),
	TEXT("B10: read what the local character has equipped in ES_BACK and confirm its mEquipmentSlot. "
	     "Requires a spawned character with a jetpack or hoverpack equipped to answer positively."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMBootProbes::ReportEquipSlot(World, &Ar);
		}));

/*
 * `FPM.Probe.ScalabilityGroups`, `FPM.Probe.BuildableTick` and `FPM.Probe.GIMethod` need no `UWorld*` -
 * every read they do is against global console state, so these three register with
 * `FAutoConsoleCommandWithOutputDevice`, the same shape `FPM.HostProbe.SelfTest` uses
 * (`FPMHostTier.cpp`), rather than carrying an unused world argument.
 */
static FAutoConsoleCommandWithOutputDevice GFPMDiagScalabilityGroupsCmd(
	TEXT("FPM.Probe.ScalabilityGroups"),
	TEXT("B4: check all 15 banked sg.* scalability groups for live existence, then diff the banked cvar "
	     "table (tools/sg_expansions.tsv) against the live console for whichever level each group is "
	     "currently at. Read-only - writes no cvar."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(
		[](FOutputDevice& Ar) { FPMBootProbes::ReportScalabilityGroups(&Ar); }));

static FAutoConsoleCommandWithOutputDevice GFPMDiagBuildableTickCmd(
	TEXT("FPM.Probe.BuildableTick"),
	TEXT("B6: search the live console for a name suggesting an independent route to "
	     "EnableBuildableTick's underlying mechanism. Read-only - writes no cvar, calls no setter."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(
		[](FOutputDevice& Ar) { FPMBootProbes::ReportBuildableTick(&Ar); }));

static FAutoConsoleCommandWithOutputDevice GFPMDiagGIMethodCmd(
	TEXT("FPM.Probe.GIMethod"),
	TEXT("B20: read r.DynamicGlobalIlluminationMethod and r.Lumen.DiffuseIndirect.Allow, each with its "
	     "GetSetBy layer. Read-only - writes no cvar."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic(
		[](FOutputDevice& Ar) { FPMBootProbes::ReportGIMethod(&Ar); }));
