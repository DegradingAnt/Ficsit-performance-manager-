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
