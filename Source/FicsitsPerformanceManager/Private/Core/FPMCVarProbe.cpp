// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.
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
// READ-ONLY. This probe sets nothing, ever. It is the instrument, not a lever -- and an instrument
// that can change what it measures is not an instrument.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMCVarWriter.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
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
