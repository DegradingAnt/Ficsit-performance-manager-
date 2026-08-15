// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMSchematicProbe.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"

#include "FGSchematic.h"
#include "FGSchematicManager.h"

#include "HAL/IConsoleManager.h"

#include <atomic>

namespace
{
	/*
	 * Four counters, and the SPLIT is the whole instrument. One aggregate would answer nothing.
	 *
	 *   Queries    — every call through the static. Proves the hook is live even when nothing is wrong,
	 *                which is the distinction the old override could never make: it emitted no armed
	 *                line, so "never fired in 13 sessions" and "never installed" looked identical.
	 *   NullCdo    — the OLD THEORY'S PREDICTED-FATAL CASE. If a 0x2c0 crash lands while this is still
	 *                zero, the null-CDO theory is dead by measurement.
	 *   NullClass  — a different failure, counted separately because the old guard conflated the two.
	 *   MgrQueries — the manager-level entry point, for comparison against the static.
	 */
	std::atomic<int32> GQueries{0};
	std::atomic<int32> GNullCdo{0};
	std::atomic<int32> GNullClass{0};
	std::atomic<int32> GMgrQueries{0};
}

FFPMSchematicProbe& FFPMSchematicProbe::Get()
{
	static FFPMSchematicProbe Instance;
	return Instance;
}

void FFPMSchematicProbe::Arm()
{
	/*
	 * HOOK 1 — `UFGSchematic::CanGiveAccessToSchematic`. This is the function that actually dies.
	 *
	 * Signature read from the header, not memory (sf-packfix step 2): FGSchematic.h:160,
	 *   static bool CanGiveAccessToSchematic( TSubclassOf< UFGSchematic > inClass, UObject* worldContext );
	 * public, STATIC, UFUNCTION(BlueprintPure), and nothing in FactoryGame overrides it — so plain
	 * SUBSCRIBE_METHOD is correct, not the _VIRTUAL form.
	 *
	 * HOOKABILITY IS PROVEN BY THE CRASH DUMPS THEMSELVES rather than assumed: they show
	 * `FicsitPerformanceManager!HookInvokerExecutorGlobalFunction<bool (__cdecl*)(TSubclassOf<UFGSchematic>...`
	 * as a live frame, so funchook has patched this target successfully in the field. That is a stronger
	 * answer to the size question than reading the prologue.
	 */
	auto OnSchematicQuery = [](auto& Scope, TSubclassOf<UFGSchematic> InClass, UObject* WorldContext)
	{
		/*
		 * ⚠ HOT PATH. The HUB and the build-menu search each enumerate every schematic in the game, and
		 * the previous attempt at this instrument froze the game by logging on every call. Everything
		 * below the counter is a pointer compare; no FString, no GetName(), no I/O, unless something
		 * anomalous is actually found.
		 */
		const int32 N = ++GQueries;

		if (!InClass)
		{
			const int32 K = ++GNullClass;
			/*
			 * CHEAP TEST FIRST, everywhere in this file and the sibling fixes. The modulo is a register
			 * operation; `IsOn` reads a console variable. Ordering them this way is free at any call
			 * volume, which is the whole justification — it does not rest on a number.
			 *
			 * ⚠ AND IT DELIBERATELY DOES NOT CLAIM ONE. An earlier draft of this comment said "the HUB
			 * walks the lot every time it opens", inferring a CALL RATE from an ASSET COUNT. Those are
			 * different things and the second was never measured. What IS measured, from the game export
			 * by reading contents rather than filenames: the Schematics tree holds 623 .json assets, of
			 * which 611 are BlueprintGeneratedClasses whose Super is FGSchematic. That bounds how many
			 * schematics EXIST. It says nothing about how often this function is called.
			 *
			 * GQueries is the answer to that, and one boot produces it.
			 */
			if ((K == 1 || (K % FPMLog::ThrottleNotable) == 0)
				&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicProbe))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-probe: a NULL schematic class reached CanGiveAccessToSchematic "
					     "(#%d of %d calls). Passed through untouched - this probe changes no answer."),
					K, N);
			}
			return;
		}

		/*
		 * ★ THE MEASUREMENT THE WHOLE PROBE EXISTS FOR. GetDefaultObject(false) — NEVER true. The default
		 * CREATES the CDO on demand, so asking with `true` would construct objects for broken schematics
		 * as a side effect of the question, and a guard that mutates state to answer is not a guard.
		 *
		 * Unthrottled on purpose. This is believed-unreachable-and-fatal: if it fires, it names the
		 * schematic the old theory says is about to kill the process, and one line is worth more than a
		 * hundred summaries. If it NEVER fires and a 0x2c0 crash still lands, the theory is finished.
		 */
		if (InClass->GetDefaultObject(/*bCreateIfNeeded=*/false) == nullptr)
		{
			const int32 K = ++GNullCdo;
			/*
			 * Silenced by FPM.Diag.Schematic 0 like everything else, DELIBERATELY. A switch that says
			 * "silence all" and then keeps talking is worse than no switch: the next reader concludes
			 * from a quiet log that nothing happened. The COUNTER still climbs either way, so
			 * FPM.Diag.List can still report it after the fact.
			 */
			if (FPMDiag::IsOn(FPMDiag::EChannel::SchematicProbe))
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] schematic-probe: %s has a NULL DEFAULT OBJECT (#%d of %d calls). This is the "
					     "case the retired 0.58.52 guard was built on. NOT refused - vanilla answers as it "
					     "would without the mod. If a 0x2c0 crash follows this line, the null-CDO theory is "
					     "confirmed; if a crash lands and this line never appeared, it is dead."),
					*InClass->GetName(), K, N);
			}
			return;
		}

		// Ordinary call. Counted, silent, and passed through: no Override, no Cancel, no change.
		if ((N == 1 || (N % FPMLog::ThrottleRoutine) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicProbe, 2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] schematic-probe: %d call(s) seen, %d null class, %d null CDO"),
				N, GNullClass.load(), GNullCdo.load());
		}
	};

	SchematicQueryHandle = FPM_SUBSCRIBE("schematic-probe", UFGSchematic::CanGiveAccessToSchematic, OnSchematicQuery);

	/*
	 * HOOK 2 — `AFGSchematicManager::CanGiveAccessToSchematic`, FGSchematicManager.h:314:
	 *   bool CanGiveAccessToSchematic( TSubclassOf< UFGSchematic > schematic ) const;
	 * public, non-virtual, and NOT a UFUNCTION. Counted only.
	 *
	 * WHY BOTH: the old mod put its forced-TRUE override here while the crash is in the static above.
	 * Watching the two side by side is what shows whether the manager-level query is even on the path
	 * to the fatal one, which nothing on record currently establishes.
	 */
	auto OnManagerQuery = [](auto& Scope, const AFGSchematicManager* Self, TSubclassOf<UFGSchematic> Schematic)
	{
		const int32 N = ++GMgrQueries;
		if ((N == 1 || (N % FPMLog::ThrottleRoutine) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicProbe, 2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] schematic-probe: manager-level query #%d (static path has seen %d)"),
				N, GQueries.load());
			FPMOverlay::Post(TEXT("schematic-probe"),
				FString::Printf(TEXT("static %d · manager %d · null-class %d · NULL-CDO %d"),
					GQueries.load(), N, GNullClass.load(), GNullCdo.load()));
		}
	};

	ManagerQueryHandle = FPM_SUBSCRIBE("schematic-probe", AFGSchematicManager::CanGiveAccessToSchematic, OnManagerQuery);

	/*
	 * ★ THE ARMED LINE — THE ONE THING THE OLD OVERRIDE NEVER HAD.
	 *
	 * Its absence is precisely why "never fired in 13 sessions" could not be told apart from "never
	 * installed", and why a feature nobody could prove was running survived three narrowings. Anything
	 * that claims to observe must first prove it is there.
	 */
	/*
	 * ⚠ BOTH handles, not one. This fix installs TWO hooks, and an earlier version of this gate
	 * checked only the manager one. That reported full ARMED while the schematic-side hook had
	 * failed, and the log text below claims coverage of "both" entry points, so a half-installed
	 * probe read as a whole one. That is the same never-fired-versus-never-installed ambiguity this
	 * ARMED line exists to kill, reintroduced one hook smaller. It fails OPEN, so it is gated on
	 * the conjunction and the failure branch names WHICH hook is missing.
	 */
	if (SchematicQueryHandle.IsValid() && ManagerQueryHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] schematic-probe ARMED - LOG ONLY on both CanGiveAccessToSchematic entry points. It "
			     "overrides nothing and refuses nothing; every answer is vanilla's. Watching for a null "
			     "default object, which is the theory the retired guard rested on."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] schematic-probe NOT fully armed - UFGSchematic hook %s, AFGSchematicManager hook %s. "
			     "Coverage is PARTIAL or absent, so a quiet session does not mean a clean one this time."),
			SchematicQueryHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"),
			ManagerQueryHandle.IsValid() ? TEXT("ok") : TEXT("FAILED"));
	}
}

void FFPMSchematicProbe::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct here even though both Arm() subscribes used plain SUBSCRIBE_METHOD,
	 * not the _VIRTUAL form (see the note at Arm(), above): both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps alike,
	 * uninstalling the detour once both are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (SchematicQueryHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGSchematic::CanGiveAccessToSchematic, SchematicQueryHandle);
		SchematicQueryHandle.Reset();
	}
	if (ManagerQueryHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(AFGSchematicManager::CanGiveAccessToSchematic, ManagerQueryHandle);
		ManagerQueryHandle.Reset();
	}
}

void FFPMSchematicProbe::GetCounts(int32& OutQueries, int32& OutMgrQueries, int32& OutNullClass, int32& OutNullCdo)
{
	OutQueries = GQueries.load();
	OutMgrQueries = GMgrQueries.load();
	OutNullClass = GNullClass.load();
	OutNullCdo = GNullCdo.load();
}

void FFPMSchematicProbe::LogReport(FOutputDevice* Ar)
{
	int32 Queries = 0, MgrQueries = 0, NullClass = 0, NullCdo = 0;
	GetCounts(Queries, MgrQueries, NullClass, NullCdo);

	const FString Line = FString::Printf(
		TEXT("[FPM] schematic-probe: %d call(s) seen on the static CanGiveAccessToSchematic path (%d on "
		     "the manager path) - %d null class, %d NULL DEFAULT OBJECT. Queries is the denominator: 0 "
		     "means the probe has not been exercised yet, not that nothing is wrong. NullCdo is the "
		     "retired guard's predicted-fatal case - if it is non-zero and no 0x2c0 crash follows, the "
		     "theory is dead; if a crash lands while it is still zero, the theory is dead the other way."),
		Queries, MgrQueries, NullClass, NullCdo);

	if (Ar != nullptr)
	{
		Ar->Log(Line);
	}
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
}

/*
 * `FPM.SchematicProbe.Report` — takes the output device so it prints in the console she is looking at
 * as well as the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command
 * that answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMSchematicProbeReportCmd(
	TEXT("FPM.SchematicProbe.Report"),
	TEXT("Print how many CanGiveAccessToSchematic calls the probe has seen on each of its two hooks, and "
	     "its null-class / null-default-object anomaly counts, this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMSchematicProbe::LogReport(&Ar);
	}));
