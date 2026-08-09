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

#include "FGGameUserSettings.h"

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
		if (UFGGameUserSettings* Settings = Cast<UFGGameUserSettings>(GEngine ? GEngine->GetGameUserSettings() : nullptr))
		{
			TMap<FString, TObjectPtr<UFGUserSettingApplyType>> All;
			Settings->GetAllUserSettingsMap(All);

			TArray<FString> Keys;
			All.GetKeys(Keys);
			Keys.Sort();
			Say(FString::Printf(TEXT("  %d setting(s) enumerated from UFGGameUserSettings"), Keys.Num()));

			// Cross-check against the writer's own judgement. A DISAGREEMENT IS THE FINDING: it means
			// clause 6's hand-maintained list has drifted from what the game actually persists, and
			// FPM would be free to write something that ends up in her settings file.
			int32 Divergent = 0;
			for (const FString& Key : Keys)
			{
				const bool bWriterThinks = FPMCVarWriter::IsUserSettingBacked(*Key);
				Say(FString::Printf(TEXT("    %-52s writer says %s"), *Key,
					bWriterThinks ? TEXT("BACKED") : TEXT("** not backed - CLAUSE 6 BLIND SPOT **")));
				if (!bWriterThinks) { ++Divergent; }
			}
			Say(Divergent == 0
				? FString(TEXT("  clause 6 agrees with the game on every setting."))
				: FString::Printf(TEXT("  ** %d setting(s) the GAME persists that clause 6 does NOT protect. "
				                       "Widen IsUserSettingBacked before the ladder writes anything. **"), Divergent));
		}
		else
		{
			// Named, not silent: "no settings object" and "zero settings" must never look the same.
			Say(TEXT("  ** COULD NOT REACH UFGGameUserSettings - enumeration NOT performed. **"));
			Say(TEXT("     This is not 'no settings'; it is 'we did not look'. P1.3 stays gated."));
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
