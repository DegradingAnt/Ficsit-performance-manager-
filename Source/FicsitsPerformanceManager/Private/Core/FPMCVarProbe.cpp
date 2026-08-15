// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.
//
// ★★★ THE ECVF_SetByConsole EXCEPTION — RULED BY ANT 2026-08-09, RECORDED HERE.
//
// The standing law is "NEVER use ECVF_SetByConsole from mod code — it means 'the user typed this'".
// A whole-mod review flagged this file for violating it. Ant's ruling: RECORD THE EXCEPTION, KEEP BOTH.
//
// WHY THE EXCEPTION IS HONEST RATHER THAN CONVENIENT:
//   * The law's own reason is that console priority means A HUMAN TYPED IT. In `FPM.Prove` and
//     `FPM.Bisect` a human DID type it — these are console commands, they do nothing until Ant runs
//     them, and they are the only writers here.
//   * For `FPM.Prove` the console write IS THE EXPERIMENT. P1.5 Leg A's protocol says, verbatim,
//     "console-override the cvar -> confirm the console WINS". A test of whether console priority beats
//     our 0x07 cannot be written without using console priority. Removing it would not make the mod
//     safer; it would make the priority stack unverifiable.
//   * Every write here is PAIRED WITH ITS OWN Unset at console priority, so the layer is removed again
//     rather than left on the stack.
//
// ⚠ THE EXCEPTION IS THIS FILE'S DIAGNOSTIC COMMANDS ONLY. It does NOT extend to any fix, any governor
// lever, or anything that runs without Ant typing it. Those go through FPMCVarWriter at 0x07, always.
//
// FPM CVAR PROBE — read the game's ACTUAL cvar state, including the per-priority layer stack.
//
// ★ WHY THIS EXISTS, and it is a measured failure rather than a nice-to-have.
//
// On 2026-08-09 Ant lost ~65 fps in her base to `sg.ShadowQuality 3` and we spent an afternoon
// failing to name which cvar. Seven hypotheses, all wrong. The root cause of the FAILURE (not of
// the frame drop) was that every candidate list came from the ENGINE's
// `Engine/Config/BaseScalability.ini`, which is NOT the table this game applies. Proven from her
// running game: at `sg.ShadowQuality 2` the console's HISTORY readout showed
//     Constructor: 2048   Scalability: 512   Console: 1024
// for `r.Shadow.MaxResolution`, while the engine ini says `[ShadowQuality@2] ...=1024`. Satisfactory
// ships its own scalability table, there is no loose copy of it anywhere in the install, and so the
// only place the truth exists is INSIDE THE RUNNING GAME.
//
// Two things that afternoon proved, both of which this file is shaped by:
//
//   1. THE LAYER STACK IS THE DATUM, NOT THE VALUE. `r.Shadow.MaxResolution = 1024` tells you almost
//      nothing. `Scalability: 512, Console: 1024, LastSetBy: Console` tells you the game's table
//      says 512 AND that a console override is currently beating it. We would have built this
//      reading only current values if Ant had not pasted a HISTORY block.
//   2. CONSOLE WRITES OUTRANK SCALABILITY AND STICK FOR THE SESSION. Once a cvar is typed into the
//      console, `sg.*` can no longer move it. That silently contaminated hours of A/B testing --
//      "re-apply the group to reset" does not reset anything already typed. `FPM.CVarDiff` makes
//      that visible instead of invisible, because the SetBy is printed beside every value.
//
// OUTPUT GOES TO THE CONSOLE, which is the other half of the same lesson: `FPM.Changes` was written
// with UE_LOG and therefore printed to FactoryGame.log while Ant watched an empty console and
// reasonably concluded the command did nothing. Every command here takes an FOutputDevice.
//
// MOSTLY READ-ONLY, AND THE EXCEPTION IS NAMED AT THE TOP OF THIS FILE.
//
// ⚠ THIS LINE USED TO READ "This probe sets nothing, ever." That was FALSE — `FPM.Bisect` and
// `FPM.Prove` both write, deliberately, at console priority. Corrected 2026-08-09 when a whole-mod
// review caught the writes; the comment had been asserting the opposite of the code sitting under it,
// which is this project's own named worst case.
//
// The reading commands — FPM.CVars, FPM.CVarHistory, FPM.CVarSnap, FPM.CVarDiff, FPM.D0 — set nothing
// and are the instrument. The two EXPERIMENTS set and then unset, because holding a variable still is
// what they are for.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMCVarWriter.h"
#include "Core/FPMMasterSwitch.h"
#include "Core/FPMUserSettingMap.h"

#include "FGGameUserSettings.h"
// Needed to ask a setting whether it owns a cvar at all, and what that cvar is called. Reading the
// map's keys instead is what made this command's cross-check report 188 false positives.
#include "Settings/FGUserSetting.h"
#include "Settings/FGUserSettingApplyType.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"

namespace
{
	/*
	 * ⚠ EVERY NAME IN HERE IS PREFIXED. Unity builds concatenate translation units, so a file-local
	 * name is not file-local -- recorded in FPMFixContract.h after it bit this project once.
	 */

	/** The default sweep when no prefix is given: the lighting surface the shadow hunt needs. */
	const TCHAR* GFPMProbeDefaultPrefixes[] = { TEXT("r.Shadow"), TEXT("r.Light"), TEXT("r.Volumetric") };

	/** Snapshot slots for FPM.CVarSnap / FPM.CVarDiff. Name -> "value|setby". */
	TMap<FString, FString> GFPMProbeSnapA;
	TMap<FString, FString> GFPMProbeSnapB;
	FString GFPMProbeSnapALabel;
	FString GFPMProbeSnapBLabel;

	/*
	 * BOTH the console AND the log, deliberately.
	 *
	 * Ar alone reaches the console Ant is looking at; UE_LOG alone reaches the file I read
	 * afterwards. Which one is "the" output depends on who is asking, and today BOTH of us needed
	 * it -- she screenshots the console, I grep FactoryGame.log. A duplicated line costs nothing; a
	 * missing one cost an hour this afternoon when FPM.Changes appeared to do nothing.
	 */
	void FPMProbeEmit(FOutputDevice& Ar, const FString& Line)
	{
		Ar.Logf(TEXT("%s"), *Line);
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	}

	/** "1024 (Scalability)" — the value and the layer that currently owns it, which is the pair that matters. */
	FString FPMProbeDescribe(IConsoleVariable* Var)
	{
		const EConsoleVariableFlags SetBy =
			static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);
		return FString::Printf(TEXT("%s (%s)"), *Var->GetString(), GetConsoleVariableSetByName(SetBy));
	}

	/** Collect every VARIABLE (not command) whose name starts with Prefix. */
	void FPMProbeCollect(const TCHAR* Prefix, TMap<FString, FString>& Out)
	{
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
			FConsoleObjectVisitor::CreateLambda([&Out](const TCHAR* Name, IConsoleObject* Obj)
			{
				// Commands have no value to snapshot. Skipping them silently is correct here — they
				// are a different KIND of object, not a variable we failed to read.
				if (IConsoleVariable* Var = Obj ? Obj->AsVariable() : nullptr)
				{
					Out.Add(FString(Name), FPMProbeDescribe(Var));
				}
			}), Prefix);
	}

	/** Prefixes from the command line, or the built-in lighting set when none were given. */
	void FPMProbeCollectAll(const TArray<FString>& Prefixes, TMap<FString, FString>& Out)
	{
		if (Prefixes.Num() == 0)
		{
			for (const TCHAR* P : GFPMProbeDefaultPrefixes) { FPMProbeCollect(P, Out); }
			return;
		}
		for (const FString& P : Prefixes) { FPMProbeCollect(*P, Out); }
	}

	FString FPMProbeJoin(const TArray<FString>& Prefixes)
	{
		if (Prefixes.Num() == 0) { return TEXT("r.Shadow* r.Light* r.Volumetric* (default set)"); }
		return FString::Join(Prefixes, TEXT(" "));
	}
}

/*
 * `FPM.CVars [prefix ...]` — every matching cvar with its value and the layer that owns it.
 */
static FAutoConsoleCommandWithArgsAndOutputDevice GFPMProbeListCmd(
	TEXT("FPM.CVars"),
	TEXT("List console variables and the priority layer that currently owns each. "
	     "Usage: FPM.CVars [prefix ...]  (default: r.Shadow r.Light r.Volumetric)"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			TMap<FString, FString> Found;
			FPMProbeCollectAll(Args, Found);
			Found.KeySort([](const FString& A, const FString& B) { return A < B; });

			FPMProbeEmit(Ar, FString::Printf(TEXT("---- cvars matching %s : %d ----"),
				*FPMProbeJoin(Args), Found.Num()));
			for (const TPair<FString, FString>& Pair : Found)
			{
				FPMProbeEmit(Ar, FString::Printf(TEXT("  %-52s = %s"), *Pair.Key, *Pair.Value));
			}
			// An empty result is a RESULT. A prefix that matches nothing and a command that did not
			// run look identical otherwise, which is the failure FPM.Changes shipped with.
			if (Found.Num() == 0)
			{
				FPMProbeEmit(Ar, TEXT("  (nothing matched - check the prefix; this command did run)"));
			}
		}));

/*
 * `FPM.Help` — the whole `FPM.*` console surface, derived live so it can never go stale.
 *
 * ★ WHY THIS AND NOT A HAND-WRITTEN LIST. A typed list of every FPM command is wrong the moment a
 * command is added, removed or renamed, and this project has already been bitten by exactly that
 * shape more than once. This walks the SAME live registration table `FPM.CVars` already walks
 * (`IConsoleManager::Get().ForEachConsoleObjectThatStartsWith`, see `FPMProbeCollect` above) —
 * just without that command's `AsVariable()`-only filter, which is what makes `FPM.CVars` blind to
 * every command on this surface by design (its own comment says so: "Commands have no value to
 * snapshot"). Nothing here is typed in twice; it reads the registration table, so it cannot drift
 * from it.
 *
 * Every FPM command and cvar already carries real TEXT help at its own registration site, so
 * `GetHelp()` is not a guess here — it is the same string the engine's own `help <name>` would show,
 * gathered for the whole `FPM.` surface in one place instead of one name at a time.
 *
 * Commands and variables are printed in SEPARATE labelled groups rather than one merged list,
 * because they answer different questions (a command DOES something when typed; a variable HOLDS a
 * value) and `IConsoleObject` cannot always be classified as one or the other — `AsCommand()` and
 * `AsVariable()` can both come back null for an exotic registration. That case is counted, not
 * guessed at or silently dropped.
 */
static FAutoConsoleCommandWithOutputDevice GFPMHelpCmd(
	TEXT("FPM.Help"),
	TEXT("List every registered FPM.* console object - commands and variables, in labelled groups, "
	     "derived live from the console registration table. Never a hand-maintained list."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		TArray<TPair<FString, FString>> Commands;    // name -> help
		TArray<TPair<FString, FString>> Variables;   // name -> help
		int32 Unclassified = 0;

		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
			FConsoleObjectVisitor::CreateLambda([&Commands, &Variables, &Unclassified](const TCHAR* Name, IConsoleObject* Obj)
			{
				if (!Obj) { return; }
				const FString Help = Obj->GetHelp();
				if (Obj->AsCommand())
				{
					Commands.Add(TPair<FString, FString>(FString(Name), Help));
				}
				else if (Obj->AsVariable())
				{
					Variables.Add(TPair<FString, FString>(FString(Name), Help));
				}
				else
				{
					// Neither cast succeeded. Counted so the total still adds up, not dropped silently -
					// an object this probe cannot classify is a finding about the probe, not a reason to
					// under-report what is actually registered.
					++Unclassified;
				}
			}), TEXT("FPM."));

		auto ByName = [](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
			{ return A.Key < B.Key; };
		Commands.Sort(ByName);
		Variables.Sort(ByName);

		const int32 Total = Commands.Num() + Variables.Num() + Unclassified;
		FString Header = FString::Printf(
			TEXT("---- FPM.Help: %d object(s) registered under 'FPM.' -- %d command(s), %d variable(s) ----"),
			Total, Commands.Num(), Variables.Num());
		if (Unclassified > 0)
		{
			Header += FString::Printf(TEXT(" (%d unclassified - neither AsCommand() nor AsVariable())"), Unclassified);
		}
		FPMProbeEmit(Ar, Header);

		FPMProbeEmit(Ar, FString::Printf(TEXT("-- COMMANDS (%d) --"), Commands.Num()));
		for (const TPair<FString, FString>& C : Commands)
		{
			FPMProbeEmit(Ar, FString::Printf(TEXT("  %-34s %s"), *C.Key, *C.Value));
		}

		FPMProbeEmit(Ar, FString::Printf(TEXT("-- VARIABLES (%d) --"), Variables.Num()));
		for (const TPair<FString, FString>& V : Variables)
		{
			FPMProbeEmit(Ar, FString::Printf(TEXT("  %-34s %s"), *V.Key, *V.Value));
		}
	}));

/*
 * `FPM.CVarHistory <name>` — the FULL priority stack for one cvar.
 *
 * This is the readout that broke the shadow investigation open. IConsoleVariable::LogHistory is
 * public (IConsoleManager.h:658) and prints every layer that has ever set the variable, with its
 * value — Constructor, SystemSettingsIni, Scalability, Console — so it reveals THIS GAME's
 * scalability table one cvar at a time, from the running process rather than from any ini.
 */
static FAutoConsoleCommandWithArgsAndOutputDevice GFPMProbeHistoryCmd(
	TEXT("FPM.CVarHistory"),
	TEXT("Print the full priority-layer history for one console variable. Usage: FPM.CVarHistory <name>"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() != 1)
			{
				FPMProbeEmit(Ar, TEXT("usage: FPM.CVarHistory <cvar name>"));
				return;
			}
			IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*Args[0]);
			if (!Var)
			{
				// Names the thing that was not found. "not found" without the name sends the reader
				// hunting their own typo in a log they may be reading hours later.
				FPMProbeEmit(Ar, FString::Printf(TEXT("no such console VARIABLE: '%s' "
					"(it may exist as a console COMMAND, which has no value or history)"), *Args[0]));
				return;
			}
			FPMProbeEmit(Ar, FString::Printf(TEXT("---- %s = %s ----"), *Args[0], *FPMProbeDescribe(Var)));
			Var->LogHistory(Ar);   // Constructor / SystemSettingsIni / Scalability / Console, with values
		}));

/*
 * `FPM.CVarSnap <A|B> [prefix ...]` then `FPM.CVarDiff`.
 *
 * The intended use, and the thing an afternoon of manual bisecting could not do:
 *     sg.ShadowQuality 2   ->  FPM.CVarSnap A
 *     sg.ShadowQuality 3   ->  FPM.CVarSnap B
 *     FPM.CVarDiff
 * That prints exactly what the group changed between the two levels, in THIS game, with the owning
 * layer beside each value. It reconstructs the real scalability table instead of guessing at it.
 */
static FAutoConsoleCommandWithArgsAndOutputDevice GFPMProbeSnapCmd(
	TEXT("FPM.CVarSnap"),
	TEXT("Capture a cvar snapshot into slot A or B for FPM.CVarDiff. "
	     "Usage: FPM.CVarSnap <A|B> [prefix ...]"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			if (Args.Num() < 1)
			{
				FPMProbeEmit(Ar, TEXT("usage: FPM.CVarSnap <A|B> [prefix ...]"));
				return;
			}
			const bool bIsA = Args[0].Equals(TEXT("A"), ESearchCase::IgnoreCase);
			const bool bIsB = Args[0].Equals(TEXT("B"), ESearchCase::IgnoreCase);
			if (!bIsA && !bIsB)
			{
				FPMProbeEmit(Ar, FString::Printf(TEXT("first argument must be A or B, got '%s'"), *Args[0]));
				return;
			}

			TArray<FString> Prefixes(Args);
			Prefixes.RemoveAt(0);

			TMap<FString, FString>& Slot = bIsA ? GFPMProbeSnapA : GFPMProbeSnapB;
			FString& Label = bIsA ? GFPMProbeSnapALabel : GFPMProbeSnapBLabel;
			Slot.Reset();
			FPMProbeCollectAll(Prefixes, Slot);
			Label = FPMProbeJoin(Prefixes);

			FPMProbeEmit(Ar, FString::Printf(TEXT("snapshot %s captured: %d cvar(s) matching %s"),
				bIsA ? TEXT("A") : TEXT("B"), Slot.Num(), *Label));
		}));

/*
 * ★★★ `FPM.D0` — THE WHOLE PHASE-2 DISCOVERY READ, ONE COMMAND, ZERO TYPING.
 *
 * Ant's standing rule, 2026-08-09: *"automate as much as possible by default. i dont like running
 * around throwing commands around."* Design R2 §9's D0-client lists a page of console reads; this is
 * all of them, plus the thing they gate.
 *
 * ★ THE PRIZE IS THE US_* ENUMERATION, because P1.3 IS GATED ON IT. The writer's clause 6 refuses to
 * touch any cvar the game's own settings save would capture, and until now `IsUserSettingBacked` has
 * had to judge that from a HAND-MAINTAINED list — a list that is wrong the moment Coffee Stain adds a
 * setting, and wrong SILENTLY. `UFGGameUserSettings::GetAllUserSettingsMap` (FGGameUserSettings.h:330)
 * is the game's own answer, and its keys are cvar names: her GameUserSettings.ini carries entries like
 * `sg.ShadowQuality`, `r.VolumetricCloud`, `FG.PlayerRules.KeepInventory`. So this replaces a guess
 * with an enumeration, from the running process.
 *
 * IT SAYS WHAT IT COULD NOT COVER. D0 also wants a ten-dismantle field watch, pop-in and connector
 * observation, and a hypertube ride — none of which code can do. Listing them is not padding: an
 * automated report that quietly omits the manual half reads as a complete discovery pass, and the
 * next session plans against it. The gap has to be visible IN the output.
 *
 * READ-ONLY. Every line here reads; nothing is set. The one thing it deliberately does NOT do is flip
 * `r.MegaLights.EnableForProject`, because that changes rendering state and belongs to a decision,
 * not a survey.
 */
static FAutoConsoleCommandWithOutputDevice GFPMD0Cmd(
	TEXT("FPM.D0"),
	TEXT("Run the whole Phase-2 discovery read in one go: US_* setting enumeration, the GI/shadow "
	     "cvar stacks, and the MegaLights prerequisites. Read-only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		auto Say = [&Ar](const FString& L)
			{ Ar.Logf(TEXT("%s"), *L); UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *L); };

		Say(TEXT("==== FPM.D0 : Phase 2 discovery read ===="));

		// ---- 1. THE US_* ENUMERATION. P1.3's gate. -------------------------------------------
		Say(TEXT(""));
		Say(TEXT("-- user settings the game's own save would capture (P1.3's gate) --"));

		// Re-read before auditing. Otherwise the cross-check below grades clause 6 against whatever
		// picture the last world load happened to leave behind, which is the wrong thing to grade it on
		// if the player has since loaded a save with a different mod set.
		FPMUserSettingMap::Refresh();

		if (UFGGameUserSettings* Settings = Cast<UFGGameUserSettings>(GEngine ? GEngine->GetGameUserSettings() : nullptr))
		{
			TMap<FString, TObjectPtr<UFGUserSettingApplyType>> All;
			Settings->GetAllUserSettingsMap(All);

			/*
			 * ⚠ CORRECTED 2026-08-09. THIS BLOCK USED TO TREAT EVERY MAP KEY AS A CVAR NAME.
			 *
			 * The keys are SETTING IDS. A setting only owns a cvar when it opts in (`UseCVar`), and then
			 * the cvar's name is its `StrId` (FGUserSetting.h:183-189). A live count taken 2026-08-09
			 * found 66 of 254 settings cvar-backed, so the old loop reported the other 188 as "CLAUSE 6
			 * BLIND SPOT". ⚠ THAT 66 DISAGREES BY ONE with FPMUserSettingTable.g.h, a generated table
			 * built from a separate FModel export that lists 67. Neither figure has been re-verified
			 * against a fresh live boot since, so read 66 and 67 as UNRECONCILED - one wrong export, one
			 * stale extraction, or a genuine one-setting version drift between the two captures - not as
			 * two independent confirmations of the same number. A diagnostic that cries wolf 188 times is
			 * worse than no diagnostic: a real blind spot becomes unfindable in the noise, and the total
			 * reads as catastrophic when almost all of it is settings that drive no cvar at all.
			 */
			int32 CVarBacked = 0;
			int32 NoCVar = 0;
			int32 Unreadable = 0;
			int32 Divergent = 0;

			TArray<FString> Backed;
			Backed.Reserve(All.Num());

			for (const TPair<FString, TObjectPtr<UFGUserSettingApplyType>>& Pair : All)
			{
				const UFGUserSettingApplyType* Apply = Pair.Value;
				const UFGUserSetting* Setting = Apply ? Apply->GetUserSetting() : nullptr;
				if (!Setting) { ++Unreadable; continue; }

				// The predicate the old loop was missing.
				if (!Setting->ShouldUseCVar()) { ++NoCVar; continue; }

				++CVarBacked;
				// StrId, not Pair.Key: two settings may drive one cvar (FGOptionInterfaceImpl.h:30-33),
				// and that duplicate should collapse rather than be reported as two findings.
				Backed.AddUnique(Setting->StrId);
			}

			Backed.Sort();
			Say(FString::Printf(
				TEXT("  %d setting(s) enumerated: %d cvar-backed, %d drive no cvar, %d unreadable"),
				All.Num(), CVarBacked, NoCVar, Unreadable));

			for (const FString& CVar : Backed)
			{
				const bool bProtected = FPMCVarWriter::IsUserSettingBacked(*CVar);
				Say(FString::Printf(TEXT("    %-52s clause 6 says %s"), *CVar,
					bProtected ? TEXT("BACKED") : TEXT("** NOT PROTECTED - CLAUSE 6 BLIND SPOT **")));
				if (!bProtected) { ++Divergent; }
			}

			Say(Divergent == 0
				? FString(TEXT("  clause 6 covers every cvar-backed setting the game reported."))
				: FString::Printf(TEXT("  ** %d cvar(s) the GAME persists that clause 6 does NOT protect. "
				                       "Regenerate the table (extract_user_settings.ps1) before the ladder "
				                       "writes anything. **"), Divergent));

			// The map's own state, because "0 blind spots" means something very different when the
			// runtime read has not landed and the answer came from a vanilla-only table.
			FPMUserSettingMap::LogState(&Ar);
		}
		else
		{
			// Named, not silent: "no settings object" and "zero settings" must never look the same.
			Say(TEXT("  ** COULD NOT REACH UFGGameUserSettings - enumeration NOT performed. **"));
			Say(TEXT("     This is not 'no settings'; it is 'we did not look'. P1.3 stays gated."));
		}

		/*
		 * ---- 1b. WOULD ANY OF IT ACTUALLY BE WRITTEN? The SaveSettings interceptor's core question,
		 *          asked WITHOUT WRITING ANYTHING.
		 *
		 * There is an unresolved contradiction in our own records and it decides how the interceptor must
		 * be built. `FPMCVarWriter.h:30-33` and design §2.3.6 state, from disassembly (AC4), that
		 * `FGGameUserSettings` serialises every `mUserSettings` entry on every save with NO DIRTY GATE.
		 * But the engine-side API says otherwise, in its own words:
		 *
		 *     FGUserSettingApplyType.h:101-102
		 *     "Returns a non empty FVariant if we have a value to actually save i.e the value is
		 *      different from the default value and marked as dirty"
		 *     virtual FVariant GetValueToSave() const;
		 *
		 * Both cannot be true. So ASK, rather than argue: `GetValueToSave()` is const and takes nothing,
		 * so every cvar-backed setting can be polled for what it would persist with ZERO risk to Ant's
		 * settings file. That is the whole reason this is a read and not an experiment — the obvious
		 * experiment (hold a real US_*-backed cvar, force a save, see if it stuck) deliberately risks
		 * writing to her settings, which is precisely what clause 6 exists to prevent.
		 *
		 * HOW TO READ THE RESULT:
		 *  - every cvar-backed setting reports EMPTY  => a dirty gate exists and is closed. A value FPM
		 *    holds on a cvar cannot leak through the save unless something also marks the setting dirty,
		 *    and the interceptor's job shrinks to "never mark dirty".
		 *  - some report a value                      => those are settings Ant has genuinely changed.
		 *    Whether OUR cvar write can make one non-empty is the follow-up, and it needs the write test.
		 *  - ALL report a value                       => the no-dirty-gate reading is right and the
		 *    interceptor must restore-before-serialise exactly as §2.3.6 specifies.
		 */
		Say(TEXT(""));
		Say(TEXT("-- would the settings save actually WRITE any of these? (GetValueToSave, read-only) --"));
		if (UFGGameUserSettings* SaveProbe = Cast<UFGGameUserSettings>(GEngine ? GEngine->GetGameUserSettings() : nullptr))
		{
			TMap<FString, TObjectPtr<UFGUserSettingApplyType>> ToSaveMap;
			SaveProbe->GetAllUserSettingsMap(ToSaveMap);

			int32 CVarBackedSeen = 0;
			int32 WouldWrite = 0;

			for (const TPair<FString, TObjectPtr<UFGUserSettingApplyType>>& Pair : ToSaveMap)
			{
				const UFGUserSettingApplyType* Apply = Pair.Value;
				const UFGUserSetting* Setting = Apply ? Apply->GetUserSetting() : nullptr;
				if (!Setting || !Setting->ShouldUseCVar()) { continue; }

				++CVarBackedSeen;

				const FVariant Pending = Apply->GetValueToSave();
				if (Pending.GetType() == EVariantTypes::Empty) { continue; }

				++WouldWrite;
				Say(FString::Printf(TEXT("    %-44s WOULD WRITE '%s'   (applied '%s', default '%s')"),
					*Setting->StrId,
					*UFGUserSettingApplyType::VariantAsString(Pending),
					*UFGUserSettingApplyType::VariantAsString(Apply->GetAppliedValue()),
					*UFGUserSettingApplyType::VariantAsString(Apply->GetDefaultValue())));
			}

			Say(FString::Printf(TEXT("  %d of %d cvar-backed settings would be written by a save right now"),
				WouldWrite, CVarBackedSeen));
			Say(WouldWrite == 0
				? FString(TEXT("  => a DIRTY GATE EXISTS AND IS CLOSED. Nothing here persists unless something "
				               "marks it dirty first. The interceptor's job may be 'never mark dirty' rather "
				               "than 'restore before serialise'. NOT yet proof that OUR write cannot dirty one."))
				: FString(TEXT("  => these carry a value the save would persist. If FPM ever holds one of THESE "
				               "cvars, the interceptor is mandatory before the write, not after.")));
		}
		else
		{
			Say(TEXT("  ** COULD NOT REACH UFGGameUserSettings - save probe NOT performed. **"));
		}

		// ---- 2. The cvar reads D0 asks for, with their full layer stacks. ---------------------
		Say(TEXT(""));
		Say(TEXT("-- D0 cvar reads (value, owning layer, full history) --"));
		const TCHAR* Wanted[] = {
			TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ContactShadows"),
			TEXT("r.DistanceFieldShadowing"), TEXT("r.DFShadowQuality"),
			TEXT("r.AOGlobalDFResolution"), TEXT("r.Shadow.Virtual.Enable"),
			TEXT("r.RayTracing"), TEXT("r.MegaLights.EnableForProject"), TEXT("r.MegaLights.Allowed"),
		};
		for (const TCHAR* Name : Wanted)
		{
			IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
			if (!V) { Say(FString::Printf(TEXT("  %-42s (no such cvar on this build)"), Name)); continue; }
			Say(FString::Printf(TEXT("  %-42s = %s"), Name, *FPMProbeDescribe(V)));
			V->LogHistory(Ar);
		}

		// ---- 3. THE HALF CODE CANNOT DO. Stated, or its absence reads as coverage. ------------
		Say(TEXT(""));
		Say(TEXT("-- NOT COVERED BY THIS COMMAND, and D0 is not complete without them --"));
		Say(TEXT("   * ten-dismantle baseline, watching the field for wrong-mesh removals (gates P3.1)"));
		Say(TEXT("   * pop-in / placed-faces / connector observation"));
		Say(TEXT("   * one hypertube ride with HypertubeDirectionProtocol disabled (I12 discriminator)"));
		Say(TEXT("   * 'stat rhi' - and STATS is compiled out of this shipping build, so it may be dead"));
		Say(TEXT("==== end FPM.D0 ===="));
	}));

/*
 * ★★★ `FPM.Bisect` — FIND WHICH CVAR IN A SCALABILITY GROUP COSTS THE FRAME. ONE COMMAND.
 *
 * Ant, 2026-08-09, after an afternoon of doing this by hand: *"no more command spam. lets build the
 * mod so it can do this itself. this is what the bench is for anyways."*
 *
 * WHAT THAT AFTERNOON PROVED, and every line here is shaped by one of these:
 *
 *   * A GROUP IS NOT A LEVER. `sg.ShadowQuality 3 -> 2` moved TEN cvars in her running game. Knowing
 *     the group costs 65 fps tells you nothing about which lever to build a ladder on.
 *   * THE ENGINE'S INI IS NOT THE GAME'S TABLE. We bisected against `BaseScalability.ini` for hours;
 *     three of six candidates did not exist in Satisfactory's table at all. This reads the LIVE cvars
 *     before and after, so no ini can mislead it.
 *   * CONSOLE WRITES STICK AND OUTRANK SCALABILITY. Typing a cvar makes it immune to `sg.*` for the
 *     session, which silently contaminated hours of A/B. This uses console priority DELIBERATELY --
 *     it is the only way to beat the group's own value -- and `Unset`s each one before the next, so
 *     candidates cannot accumulate.
 *   * A HUMAN CANNOT HOLD THE VARIABLES STILL. Time of day, warm-up, camera framing and n=1 eyeballing
 *     invalidated a whole session. This samples wall-clock frame time over a fixed window, in one
 *     place, seconds apart.
 *
 * WALL CLOCK, NOT `FApp::GetDeltaTime()`. That is smoothed and clamped by `UEngine::bSmoothFrameRate`
 * (`Engine.h:1552`), so it would flatten exactly the differences being measured. Same finding the
 * hitch meter is built on.
 *
 * IT REPORTS WHAT IT CANNOT ACCOUNT FOR. If the individual recoveries do not add up to the whole
 * gap, that is printed as UNACCOUNTED rather than hidden -- it means either the cvars interact, or
 * the real cost is not in the set at all. Same discipline as the rain sweep's bucket check, which
 * caught a real 48-class gap on its first run.
 */
namespace
{
	enum class EFPMBisectStep : uint8 { WarmGood, SampleGood, WarmBad, SampleBad, WarmCand, SampleCand, Done };

	struct FFPMBisectRun
	{
		FString GroupCVar;
		int32   GoodLevel = 0;
		int32   BadLevel  = 0;

		TArray<FString> Names;        // cvars that differ between the two levels
		TArray<FString> GoodValues;   // parallel: value at the GOOD level

		EFPMBisectStep Step = EFPMBisectStep::Done;
		int32  Index = 0;
		double PhaseEnd = 0.0;
		double LastTick = 0.0;
		int32  Frames = 0;
		double Accum = 0.0;
		double WorstMs = 0.0;

		double GoodMs = 0.0;
		double BadMs  = 0.0;
		TArray<TPair<FString, double>> Results;   // name -> mean ms with that cvar at its GOOD value

		FTSTicker::FDelegateHandle Tick;
		bool bRunning = false;
	};

	FFPMBisectRun GFPMBisect;

	/** 1.5 s to let the renderer settle after a change, then 2.5 s of samples. */
	constexpr double GFPMBisectWarm   = 1.5;
	constexpr double GFPMBisectSample = 2.5;

	void FPMBisectSay(const FString& L) { UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *L); }

	void FPMBisectApplyGroup(int32 Level)
	{
		// Exec rather than Set(): the sg.* cvars are applied by a console-variable sink, and going
		// through the same entry point the settings menu uses is the point of the exercise.
		if (GEngine)
		{
			GEngine->Exec(nullptr, *FString::Printf(TEXT("%s %d"), *GFPMBisect.GroupCVar, Level));
		}
	}

	void FPMBisectClearCandidate()
	{
		if (GFPMBisect.Names.IsValidIndex(GFPMBisect.Index))
		{
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(*GFPMBisect.Names[GFPMBisect.Index]))
			{
				// Unset by PRIORITY. Setting the old value back would append another history layer and
				// leave ours owning the variable -- the same reason the writer's release uses Unset.
				V->Unset(ECVF_SetByConsole);
			}
		}
	}

	void FPMBisectStop(bool bRestoreGroup)
	{
		FPMBisectClearCandidate();
		if (bRestoreGroup) { FPMBisectApplyGroup(GFPMBisect.BadLevel); }
		if (GFPMBisect.Tick.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GFPMBisect.Tick);
			GFPMBisect.Tick.Reset();
		}
		GFPMBisect.bRunning = false;
		GFPMBisect.Step = EFPMBisectStep::Done;
	}

	void FPMBisectReport()
	{
		const double Gap = GFPMBisect.BadMs - GFPMBisect.GoodMs;
		FPMBisectSay(TEXT(""));
		FPMBisectSay(FString::Printf(TEXT("==== FPM.Bisect %s : %d (bad) vs %d (good) ===="),
			*GFPMBisect.GroupCVar, GFPMBisect.BadLevel, GFPMBisect.GoodLevel));
		FPMBisectSay(FString::Printf(TEXT("  whole group : %.2f ms -> %.2f ms   (gap %.2f ms, %.0f -> %.0f fps)"),
			GFPMBisect.BadMs, GFPMBisect.GoodMs, Gap,
			GFPMBisect.BadMs > 0 ? 1000.0 / GFPMBisect.BadMs : 0.0,
			GFPMBisect.GoodMs > 0 ? 1000.0 / GFPMBisect.GoodMs : 0.0));

		if (Gap <= 0.5)
		{
			// Stated, never silent. Without a gap there is nothing to attribute, and ranking noise
			// would produce a confident-looking table built entirely out of jitter.
			FPMBisectSay(TEXT("  ** NO MEANINGFUL GAP between the two levels here. Nothing to attribute --"));
			FPMBisectSay(TEXT("     move somewhere the group actually costs you, and re-run."));
			return;
		}

		auto Sorted = GFPMBisect.Results;
		Sorted.Sort([](const TPair<FString, double>& A, const TPair<FString, double>& B)
			{ return A.Value < B.Value; });   // lowest ms first == biggest recovery first

		double Accounted = 0.0;
		FPMBisectSay(TEXT("  each cvar alone, set to its GOOD value while the rest stay BAD:"));
		for (const TPair<FString, double>& R : Sorted)
		{
			const double Recovered = GFPMBisect.BadMs - R.Value;
			Accounted = FMath::Max(Accounted, Recovered);   // best single, not a sum -- see below
			FPMBisectSay(FString::Printf(TEXT("    %-52s %7.2f ms   recovers %6.2f ms  (%.0f%% of the gap)"),
				*R.Key, R.Value, Recovered, 100.0 * Recovered / Gap));
		}

		// WHY "best single" AND NOT A SUM: these are not independent, so adding recoveries would
		// double-count. What matters is whether ANY ONE of them explains the gap. If the best single
		// falls well short, the cost is spread or it is not in this set -- and that is a finding, not
		// a failed run.
		FPMBisectSay(FString::Printf(TEXT("  best single cvar explains %.0f%% of the gap"),
			100.0 * Accounted / Gap));
		if (Accounted < Gap * 0.5)
		{
			FPMBisectSay(TEXT("  ** NO SINGLE CVAR EXPLAINS THE GAP. Either they interact, or the cost is"));
			FPMBisectSay(TEXT("     not in this group's diff at all. Do not pick a ladder lever off this table."));
		}
	}

	bool FPMBisectTick(float)
	{
		const double Now = FPlatformTime::Seconds();
		const double Delta = Now - GFPMBisect.LastTick;
		GFPMBisect.LastTick = Now;

		const bool bSampling = GFPMBisect.Step == EFPMBisectStep::SampleGood
		                    || GFPMBisect.Step == EFPMBisectStep::SampleBad
		                    || GFPMBisect.Step == EFPMBisectStep::SampleCand;
		if (bSampling && Delta > 0.0 && Delta < 1.0)   // >1 s is a stall, not a frame
		{
			++GFPMBisect.Frames;
			GFPMBisect.Accum += Delta;
			GFPMBisect.WorstMs = FMath::Max(GFPMBisect.WorstMs, Delta * 1000.0);
		}

		if (Now < GFPMBisect.PhaseEnd) { return true; }

		auto BeginPhase = [&](EFPMBisectStep NextStep, double Seconds)
		{
			GFPMBisect.Step = NextStep;
			GFPMBisect.PhaseEnd = Now + Seconds;
			GFPMBisect.Frames = 0;
			GFPMBisect.Accum = 0.0;
			GFPMBisect.WorstMs = 0.0;
		};
		auto MeanMs = [&]() -> double
			{ return GFPMBisect.Frames > 0 ? (GFPMBisect.Accum / GFPMBisect.Frames) * 1000.0 : 0.0; };

		switch (GFPMBisect.Step)
		{
		case EFPMBisectStep::WarmGood:
			BeginPhase(EFPMBisectStep::SampleGood, GFPMBisectSample);
			return true;

		case EFPMBisectStep::SampleGood:
		{
			GFPMBisect.GoodMs = MeanMs();
			FPMBisectSay(FString::Printf(TEXT("  good level %d : %.2f ms over %d frames"),
				GFPMBisect.GoodLevel, GFPMBisect.GoodMs, GFPMBisect.Frames));
			// Snapshot the GOOD values before switching, so the candidate list is built from live
			// state rather than from any ini.
			TMap<FString, FString> GoodSnap;
			for (const TCHAR* P : GFPMProbeDefaultPrefixes) { FPMProbeCollect(P, GoodSnap); }
			FPMProbeCollect(TEXT("r.DF"), GoodSnap);
			FPMProbeCollect(TEXT("r.AO"), GoodSnap);
			FPMProbeCollect(TEXT("r.Capsule"), GoodSnap);
			FPMProbeCollect(TEXT("r.DistanceField"), GoodSnap);
			GFPMBisect.Names.Reset(); GFPMBisect.GoodValues.Reset();
			for (const TPair<FString, FString>& Pair : GoodSnap)
			{
				if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
				{
					GFPMBisect.Names.Add(Pair.Key);
					GFPMBisect.GoodValues.Add(V->GetString());
				}
			}
			FPMBisectApplyGroup(GFPMBisect.BadLevel);
			BeginPhase(EFPMBisectStep::WarmBad, GFPMBisectWarm);
			return true;
		}

		case EFPMBisectStep::WarmBad:
			BeginPhase(EFPMBisectStep::SampleBad, GFPMBisectSample);
			return true;

		case EFPMBisectStep::SampleBad:
		{
			GFPMBisect.BadMs = MeanMs();
			FPMBisectSay(FString::Printf(TEXT("  bad  level %d : %.2f ms over %d frames"),
				GFPMBisect.BadLevel, GFPMBisect.BadMs, GFPMBisect.Frames));

			// Keep only cvars that ACTUALLY differ now. Everything else is noise in the table.
			TArray<FString> KeepN; TArray<FString> KeepV;
			for (int32 i = 0; i < GFPMBisect.Names.Num(); ++i)
			{
				IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(*GFPMBisect.Names[i]);
				if (V && V->GetString() != GFPMBisect.GoodValues[i])
				{
					KeepN.Add(GFPMBisect.Names[i]); KeepV.Add(GFPMBisect.GoodValues[i]);
				}
			}
			GFPMBisect.Names = KeepN; GFPMBisect.GoodValues = KeepV;
			FPMBisectSay(FString::Printf(TEXT("  %d cvar(s) differ between the levels; testing each alone (~%.0f s)"),
				GFPMBisect.Names.Num(), GFPMBisect.Names.Num() * (GFPMBisectWarm + GFPMBisectSample)));

			if (GFPMBisect.Names.Num() == 0)
			{
				FPMBisectSay(TEXT("  ** the group changed NOTHING in the cvars scanned. Either the prefixes"));
				FPMBisectSay(TEXT("     miss it, or console overrides are pinning them. Restart clears those."));
				FPMBisectReport();
				FPMBisectStop(false);
				return false;
			}
			GFPMBisect.Index = -1;
			BeginPhase(EFPMBisectStep::SampleCand, 0.0);   // fall through into the advance below
			GFPMBisect.Step = EFPMBisectStep::SampleCand;
			GFPMBisect.PhaseEnd = Now;   // trigger the advance immediately
			return true;
		}

		case EFPMBisectStep::WarmCand:
			BeginPhase(EFPMBisectStep::SampleCand, GFPMBisectSample);
			return true;

		case EFPMBisectStep::SampleCand:
		{
			if (GFPMBisect.Index >= 0)
			{
				GFPMBisect.Results.Emplace(GFPMBisect.Names[GFPMBisect.Index], MeanMs());
				FPMBisectClearCandidate();
			}
			++GFPMBisect.Index;
			if (!GFPMBisect.Names.IsValidIndex(GFPMBisect.Index))
			{
				FPMBisectReport();
				FPMBisectStop(true);
				return false;
			}
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(*GFPMBisect.Names[GFPMBisect.Index]))
			{
				// Console priority on purpose: it is the only layer that beats the group's own value.
				/*
				 * ⚠ THIS CONTAMINATES THE SESSION, ON PURPOSE, AND THAT IS WHY IT SAYS SO OUT LOUD.
				 * A console write STICKS for the rest of the session: once this runs, `sg.*` can no longer
				 * move these cvars. That is exactly the trap that silently invalidated hours of A/B testing
				 * on 2026-08-09 — "re-apply the group to reset" resets nothing already typed. The bisect NEEDS
				 * console priority to hold a variable still against the game's own scalability re-applies, so
				 * the contamination is the price of the instrument, not a bug in it.
				 */
				V->Set(*GFPMBisect.GoodValues[GFPMBisect.Index], ECVF_SetByConsole);
			}
			BeginPhase(EFPMBisectStep::WarmCand, GFPMBisectWarm);
			return true;
		}

		default:
			FPMBisectStop(false);
			return false;
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GFPMBisectCmd(
	TEXT("FPM.Bisect"),
	TEXT("Find which cvar in a scalability group costs the frame. "
	     "Usage: FPM.Bisect <sg.Group> <badLevel> <goodLevel>   e.g. FPM.Bisect sg.ShadowQuality 3 2"),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateLambda(
		[](const TArray<FString>& Args, FOutputDevice& Ar)
		{
			auto Say = [&Ar](const FString& L) { Ar.Logf(TEXT("%s"), *L); FPMBisectSay(L); };

			if (GFPMBisect.bRunning)
			{
				Say(TEXT("FPM.Bisect is already running. FPM.Bisect.Stop to abort."));
				return;
			}
			if (Args.Num() != 3)
			{
				Say(TEXT("usage: FPM.Bisect <sg.Group> <badLevel> <goodLevel>   e.g. FPM.Bisect sg.ShadowQuality 3 2"));
				return;
			}
			if (!IConsoleManager::Get().FindConsoleVariable(*Args[0]))
			{
				Say(FString::Printf(TEXT("no such scalability cvar: '%s'"), *Args[0]));
				return;
			}

			GFPMBisect = FFPMBisectRun();
			GFPMBisect.GroupCVar = Args[0];
			GFPMBisect.BadLevel  = FCString::Atoi(*Args[1]);
			GFPMBisect.GoodLevel = FCString::Atoi(*Args[2]);
			GFPMBisect.bRunning  = true;

			Say(FString::Printf(TEXT("---- FPM.Bisect %s : %d (bad) vs %d (good) ----"),
				*GFPMBisect.GroupCVar, GFPMBisect.BadLevel, GFPMBisect.GoodLevel));
			Say(TEXT("  STAND STILL AND DO NOT MOVE THE CAMERA until this finishes."));
			Say(TEXT("  Every sample is the same view seconds apart; moving invalidates the whole table."));
			Say(TEXT("  Results print here and to the log when it completes."));

			FPMBisectApplyGroup(GFPMBisect.GoodLevel);
			GFPMBisect.LastTick = FPlatformTime::Seconds();
			GFPMBisect.Step = EFPMBisectStep::WarmGood;
			GFPMBisect.PhaseEnd = GFPMBisect.LastTick + GFPMBisectWarm;
			GFPMBisect.Tick = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&FPMBisectTick), 0.f);   // 0 == every frame
		}));

static FAutoConsoleCommandWithOutputDevice GFPMBisectStopCmd(
	TEXT("FPM.Bisect.Stop"),
	TEXT("Abort a running FPM.Bisect and restore the group to its bad level."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		if (!GFPMBisect.bRunning) { Ar.Logf(TEXT("FPM.Bisect is not running.")); return; }
		FPMBisectStop(true);
		Ar.Logf(TEXT("FPM.Bisect aborted; candidate override cleared and group restored."));
	}));

/*
 * ★★★ `FPM.Prove` — P1.5's ENTIRE 0x07 PROOF PROTOCOL, AS ONE COMMAND.
 *
 * Ant, 2026-08-09, halfway through running it by hand: *"okey this is too many commands and its
 * confusing. we need to make the mod do this."* and *"no more command spam. lets build the mod so it
 * can do this itself. this is what the bench is for anyways."*
 *
 * She is right twice over. Hand-driving a protocol is how it goes wrong -- the same failure the
 * afternoon's manual cvar bisect demonstrated -- and every step below is something code can perform
 * more precisely than a human typing between two windows. What a human CANNOT do reliably is notice
 * that they mistyped step 4 of 6 an hour ago.
 *
 * WHY IT MATTERS: Design R2 §9's P1.5 is the last Phase 1 increment and it BLOCKS A LAW CHANGE -- the
 * recorded project law keeps prescribing SetByCode until this lands. It has been unrunnable until
 * today, because nothing made the writer hold a cvar on demand.
 *
 * THE FIVE QUESTIONS, and the priority contest each one settles:
 *   1  does a tagged FPM write actually take?
 *   2  does it SURVIVE a scalability-priority write?      <- the ladder rests on yes
 *   3  does the CONSOLE still beat us?                    <- MUST be yes, or FPM has locked
 *                                                            the player out of their own console
 *   4  does release restore the value AND the SetBy?      <- the zero-residue promise
 *   5  does it survive the vanilla options menu?          <- the path a player actually uses
 *
 * Steps 1-4 run automatically and instantly. Step 5 needs a human to open Settings and click Apply,
 * so rather than making that a second command, the probe ARMS A WATCHER and reports by itself when
 * the apply happens -- or when it times out, which is also an answer.
 *
 * IT ALWAYS CLEANS UP. Every exit path releases the hold and unsets the probe's own console write.
 * A proof that leaves residue behind has disproved the thing it set out to demonstrate.
 */
namespace
{
	/** Scalability-owned, NOT US_*-backed, and confirmed to differ across sg levels in Ant's game. */
	const TCHAR* GFPMProveTarget = TEXT("r.Shadow.MaxResolution");
	const TCHAR* GFPMProveHeld   = TEXT("777");   // absurd on purpose: unmistakable in any readout
	const TCHAR* GFPMProveRival  = TEXT("888");   // what the simulated scalability apply tries
	const TCHAR* GFPMProveOwner  = TEXT("prove");

	FTSTicker::FDelegateHandle GFPMProveWatch;
	FString  GFPMProveArmedValue;
	double   GFPMProveDeadline = 0.0;
	int32    GFPMProvePassed = 0;
	int32    GFPMProveTotal  = 0;

	void FPMProveLog(const FString& Line)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	}

	void FPMProveStep(FOutputDevice& Ar, const TCHAR* Question, bool bPass, const FString& Detail)
	{
		++GFPMProveTotal;
		if (bPass) { ++GFPMProvePassed; }
		const FString Line = FString::Printf(TEXT("  [%s] %-46s %s"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Question, *Detail);
		Ar.Logf(TEXT("%s"), *Line);
		FPMProveLog(Line);
	}

	void FPMProveCleanup()
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(GFPMProveTarget))
		{
			// Our own console-priority write from step 3 is residue if it survives the probe. Unset by
			// priority, not "set it back" -- a lower Set appends another history layer instead of
			// removing ours, which is the same reason the writer's release uses Unset.
			Var->Unset(ECVF_SetByConsole);
		}
		FPMCVarWriter::Get().ReleaseOwner(FName(GFPMProveOwner));
		if (GFPMProveWatch.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GFPMProveWatch);
			GFPMProveWatch.Reset();
		}
	}
}

/*
 * Item 2: registers THIS file's two diagnostic stop paths with the master switch, so `FPM.Off`
 * reaches an active `FPM.Bisect` or `FPM.Prove` run. A file-scope static, matching the console
 * commands below - it runs at module load, before StartupModule can ever reach the OFF branch.
 * Calls the two functions above BY NAME rather than exporting them: they stay this file's own.
 */
static const bool GFPMProbeStopHooksRegistered = []
{
	FPMMasterSwitch::RegisterStopHook([]() { FPMBisectStop(/*bRestoreGroup=*/true); });
	FPMMasterSwitch::RegisterStopHook([]() { FPMProveCleanup(); });
	return true;
}();

static FAutoConsoleCommandWithOutputDevice GFPMProveCmd(
	TEXT("FPM.Prove"),
	TEXT("Run the whole 0x07 priority proof (P1.5) and print a verdict. Cleans up after itself."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		auto Say = [&Ar](const FString& L) { Ar.Logf(TEXT("%s"), *L); FPMProveLog(L); };

		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(GFPMProveTarget);
		if (!Var)
		{
			Say(FString::Printf(TEXT("FPM.Prove ABORTED: no cvar '%s' on this build."), GFPMProveTarget));
			return;
		}

		// Anything left over from a previous run would make step 1 unreadable.
		FPMProveCleanup();
		GFPMProvePassed = GFPMProveTotal = 0;

		const FString OrigValue = Var->GetString();
		const EConsoleVariableFlags OrigSetBy =
			static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);

		Say(FString::Printf(TEXT("---- FPM.Prove : %s ----"), GFPMProveTarget));
		Say(FString::Printf(TEXT("  baseline: %s (%s)"), *OrigValue, GetConsoleVariableSetByName(OrigSetBy)));

		// 1 — does a tagged FPM write take at all?
		const bool bHeld = FPMCVarWriter::Get().Hold(FName(GFPMProveOwner), GFPMProveTarget,
			GFPMProveHeld, TEXT("FPM.Prove: 0x07 priority proof (P1.5)"));
		FPMProveStep(Ar, TEXT("1 FPM write takes"), bHeld && Var->GetString() == GFPMProveHeld,
			FString::Printf(TEXT("live=%s (%s)"), *Var->GetString(),
				GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask))));

		if (!bHeld)
		{
			Say(TEXT("  hold refused, so the remaining steps would be meaningless. Aborting and cleaning up."));
			FPMProveCleanup();
			return;
		}

		// 2 — the contest the whole ladder rests on. This is the same priority the settings menu's
		//     apply path writes at, so a loss here means the ladder cannot hold a value at all.
		Var->Set(GFPMProveRival, ECVF_SetByScalability);
		FPMProveStep(Ar, TEXT("2 survives a Scalability write"), Var->GetString() == GFPMProveHeld,
			FString::Printf(TEXT("scalability tried %s, live=%s"), GFPMProveRival, *Var->GetString()));

		/*
		 * ⚠ PUT THE SCALABILITY LAYER BACK, OR STEP 4 REPORTS A FAILURE THAT IS OURS.
		 *
		 * Found on the first real run, 2026-08-09: step 4 printed
		 *     [FAIL] release restores value AND SetBy    2048 (Scalability) -> 888 (Scalability)
		 * and the writer had done nothing wrong. `Set(..., ECVF_SetByScalability)` does not stack on
		 * top of the game's value at that priority, it REPLACES that slot -- so the line above had
		 * already overwritten the game's 2048 with our 888. Release then fell back to the Scalability
		 * layer exactly as designed, and the layer simply was not 2048 any more.
		 *
		 * A test that reports FAIL on a working system is worse than no test: it would have sent us
		 * hunting a release bug that does not exist, in the one path the zero-residue promise rests on.
		 * Unset cannot undo it either -- our write and the game's occupy the same untagged slot -- so
		 * the only honest repair is to write the original value back at the same priority.
		 */
		Var->Set(*OrigValue, ECVF_SetByScalability);

		// 3 — MUST pass. If FPM outranked the console, we would have taken away the operator's ability
		//     to override us, which is a worse outcome than any performance win.
		Var->Set(TEXT("999"), ECVF_SetByConsole);
		FPMProveStep(Ar, TEXT("3 the CONSOLE still beats FPM"), Var->GetString() == TEXT("999"),
			FString::Printf(TEXT("console tried 999, live=%s"), *Var->GetString()));
		Var->Unset(ECVF_SetByConsole);

		// 4 — the zero-residue promise, checked on BOTH axes. A value that comes back while the SetBy
		//     does not means our tag is still sitting on the variable, locking out lower writers --
		//     invisible to anyone reading only the number.
		FPMCVarWriter::Get().ReleaseOwner(FName(GFPMProveOwner));
		const FString AfterValue = Var->GetString();
		const EConsoleVariableFlags AfterSetBy =
			static_cast<EConsoleVariableFlags>(Var->GetFlags() & ECVF_SetByMask);
		FPMProveStep(Ar, TEXT("4 release restores value AND SetBy"),
			AfterValue == OrigValue && AfterSetBy == OrigSetBy,
			FString::Printf(TEXT("%s (%s) -> %s (%s)"), *OrigValue, GetConsoleVariableSetByName(OrigSetBy),
				*AfterValue, GetConsoleVariableSetByName(AfterSetBy)));

		Say(FString::Printf(TEXT("  automatic legs: %d/%d passed"), GFPMProvePassed, GFPMProveTotal));

		// 5 — the leg only a human can trigger. Rather than making this a second command she has to
		//     remember, arm a watcher and report by ourselves.
		FPMCVarWriter::Get().Hold(FName(GFPMProveOwner), GFPMProveTarget, GFPMProveHeld,
			TEXT("FPM.Prove: watching for a vanilla options-menu apply"));
		GFPMProveArmedValue = GFPMProveHeld;
		GFPMProveDeadline = FPlatformTime::Seconds() + 180.0;

		Say(TEXT(""));
		Say(TEXT("  STEP 5 - NOW OPEN SETTINGS, CHANGE ANY GRAPHICS OPTION, AND CLICK APPLY."));
		Say(TEXT("  Nothing else to type. I am holding the cvar and watching it; I will print the"));
		Say(TEXT("  verdict myself the moment the menu applies, or say so if 3 minutes pass."));

		GFPMProveWatch = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float) -> bool
			{
				IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(GFPMProveTarget);
				if (!V) { FPMProveCleanup(); return false; }

				const FString Now = V->GetString();
				if (Now != GFPMProveArmedValue)
				{
					// Something beat our hold. That IS the answer, and the SetBy names who did it.
					FPMProveLog(FString::Printf(
						TEXT("  [FAIL] 5 survives the options-menu apply       our %s -> %s (%s) "
						     "** the menu's apply path OUTRANKS 0x07 - §2.3.2's fallback engages **"),
						*GFPMProveArmedValue, *Now,
						GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(V->GetFlags() & ECVF_SetByMask))));
					FPMProveCleanup();
					return false;
				}
				if (FPlatformTime::Seconds() > GFPMProveDeadline)
				{
					// NOT a pass. "Nobody applied anything" and "the hold survived an apply" are
					// different facts and must never print the same line.
					FPMProveLog(TEXT("  [ -- ] 5 options-menu apply NOT OBSERVED in 180 s - "
						"inconclusive, not a pass. Re-run FPM.Prove and use the menu."));
					FPMProveCleanup();
					return false;
				}
				return true;
			}), 0.5f);
	}));

static FAutoConsoleCommandWithOutputDevice GFPMProbeDiffCmd(
	TEXT("FPM.CVarDiff"),
	TEXT("Print every cvar that differs between snapshots A and B, with the owning layer."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		// Both slots must exist. Diffing against an empty slot would report every cvar as "added",
		// which reads like a huge finding and is in fact an unset slot -- the silent-partial-success
		// shape this project has paid for twice.
		if (GFPMProbeSnapA.Num() == 0 || GFPMProbeSnapB.Num() == 0)
		{
			FPMProbeEmit(Ar, FString::Printf(
				TEXT("REFUSING: slot A has %d cvar(s), slot B has %d. Capture BOTH first "
				     "(FPM.CVarSnap A / FPM.CVarSnap B) - diffing against an empty slot would report "
				     "every variable as changed."), GFPMProbeSnapA.Num(), GFPMProbeSnapB.Num()));
			return;
		}
		// Comparing different prefix sets produces phantom adds and removes. Say so rather than
		// letting the reader discover it from a nonsensical diff.
		if (!GFPMProbeSnapALabel.Equals(GFPMProbeSnapBLabel))
		{
			FPMProbeEmit(Ar, FString::Printf(TEXT("WARNING: A covered '%s' but B covered '%s' - "
				"the sets differ, so additions and removals below are not meaningful."),
				*GFPMProbeSnapALabel, *GFPMProbeSnapBLabel));
		}

		TArray<FString> Names;
		for (const TPair<FString, FString>& P : GFPMProbeSnapA) { Names.AddUnique(P.Key); }
		for (const TPair<FString, FString>& P : GFPMProbeSnapB) { Names.AddUnique(P.Key); }
		Names.Sort();

		int32 Changed = 0;
		FPMProbeEmit(Ar, FString::Printf(TEXT("---- cvar diff: A (%d) -> B (%d) ----"),
			GFPMProbeSnapA.Num(), GFPMProbeSnapB.Num()));
		for (const FString& Name : Names)
		{
			const FString* A = GFPMProbeSnapA.Find(Name);
			const FString* B = GFPMProbeSnapB.Find(Name);
			if (A && B && A->Equals(*B)) { continue; }
			++Changed;
			FPMProbeEmit(Ar, FString::Printf(TEXT("  %-52s %s  ->  %s"), *Name,
				A ? **A : TEXT("(absent)"), B ? **B : TEXT("(absent)")));
		}
		// Zero differences is a real and useful answer -- it means whatever you did between the two
		// snapshots did not touch this surface at all. Stated, never silent.
		FPMProbeEmit(Ar, Changed == 0
			? FString(TEXT("  (identical - nothing in this cvar set changed between A and B)"))
			: FString::Printf(TEXT("  %d cvar(s) differ"), Changed));
	}));

/*
 * ★★★ D0 RUNS ITSELF, ONCE PER BOOT — because a console command CANNOT BE DELIVERED TO THIS GAME.
 *
 * Measured 2026-08-09, not assumed. UE strips `-ExecCmds` in Shipping and SML reimplements it
 * (`SatisfactoryModLoader.cpp:218-227`, queuing deferred commands) — but Steam replaces the command line
 * with its own launch options before the game ever sees ours. A direct launch of the exe and
 * `steam://run/526870//-ExecCmds=...` both came up as `-NO_EOS_OVERLAY -useallavailablecores` and
 * nothing else. There is no outside route in.
 *
 * That makes every typed diagnostic cost one of Ant's boots, against her standing rule: *"automate as
 * much as possible by default. i dont like running around throwing commands around."* Design R2's
 * D0-client is a page of console reads; this is how that page gets read without her hands.
 *
 * ⚠ IT DISPATCHES THE REGISTERED COMMAND rather than calling a copy of its body. `FPM.D0` stays the one
 * implementation, so the automatic report and the typed one can never drift into saying different
 * things — which is the whole failure this project keeps paying for.
 *
 * ⚠ AND IT WAITS. `OnFEngineLoopInitComplete` fires before the main menu has settled and before mods
 * have finished registering their settings; reading then would produce a confidently WRONG enumeration
 * (vanilla-only) that looks exactly like a complete one. The delay is not politeness, it is the
 * difference between a right answer and a plausible one.
 */
static TAutoConsoleVariable<int32> CVarD0Auto(
	TEXT("FPM.D0.Auto"), 1,
	TEXT("Run FPM.D0 automatically once, a few seconds after engine init, and write it to the log. "
	     "1 = on (default while the discovery phase is open), 0 = off. It only READS."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarD0AutoDelay(
	TEXT("FPM.D0.AutoDelay"), 8.0f,
	TEXT("Seconds after engine-init-complete before the automatic FPM.D0 run. Long enough for the main "
	     "menu and for mod-registered settings to exist."),
	ECVF_Default);

/** Bound once at static init; the ticker is one-shot and unregisters itself by returning false. */
static FDelegateHandle GFPMD0AutoHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddStatic([]()
{
	if (CVarD0Auto.GetValueOnGameThread() == 0) { return; }

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic([](float) -> bool
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] D0.Auto: running FPM.D0 by itself (no console input is deliverable to this "
			     "build - see the comment in FPMCVarProbe.cpp). Disable with FPM.D0.Auto 0."));

		// GLog, so the whole report lands in FactoryGame.log where it can be read after the fact.
		IConsoleManager::Get().ProcessUserConsoleInput(TEXT("FPM.D0"), *GLog, nullptr);

		return false;   // one-shot
	}), CVarD0AutoDelay.GetValueOnGameThread());
});
