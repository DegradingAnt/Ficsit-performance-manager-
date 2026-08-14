// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMFixContract.h"
#include "Core/FPMHookLedger.h"
#include "UI/FPMChatRelay.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "RHI.h"
#include "RHIStats.h"   // RHIGetTextureMemoryStats - stands in for the stripped `stat rhi`
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * ★ THE SUPPORT SURFACES — design §7.1 (`FPM.Support`), §7.10 (vanilla-defaults drift watch) and
 * §7.12 (boot-time `.uplugin` parity self-check). All three are P1.1 items and all three were missing;
 * found 2026-08-09 when Ant asked whether Phase 1 was really finished. It was not.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY §7.1 EXISTS, in the design's own words: "every support loop today is an agent hand-grepping
 * 92 MB of logs". Plus two recorded misreadings that a single authoritative block would have prevented:
 * a detector ignored for 13 days, and "FPM appears in the log" conflated with "an FPM frame is in the
 * callstack" — which produced a wrong measurement that stood until it was re-derived.
 *
 * ⚠ THIS IS A DEV/SUPPORT SURFACE, NOT A PLAYER ONE. Ant, 2026-08-09: "a player wont check logs for
 * stuff, only devs do" — and the answer to that is the chat relay, not this. A sixty-line block does
 * not belong in a chat window and chat is not copyable. So `FPM.Support` writes to the CONSOLE (where
 * it can be selected and pasted) and to the log. The drift WARN below is the opposite case: it IS
 * player-facing, so it goes to chat as well.
 */

namespace
{
	/**
	 * ★ THE CL OUR TABLES WERE DERIVED FROM. Design §7.10 names this number explicitly.
	 *
	 * WHY A DRIFT WATCH IS WORTH ITS KEEP: CL480321 changed HISM→ISM semantics SILENTLY and broke
	 * removal visuals for months before anyone connected the two. The vanilla-defaults TSV, the US_* map
	 * fallback and the scalability expansions all age exactly the same way — they are snapshots of a
	 * moving target that gives no notification when it moves. This is cheap insurance against the next
	 * CSS refactor doing the same thing.
	 *
	 * BUMPED 495413 -> 502094 on 2026-08-14, and ONLY because the tables were actually re-derived.
	 * The constant is the LAST step of a re-derivation, never the first - moving it early silences the
	 * alarm for tables nobody re-read, which is worse than no alarm at all.
	 */
	constexpr uint32 DerivedFromChangelist = 502094;

	/** What goes stale when the CL moves. Named individually so the WARN is actionable, not ominous. */
	const TCHAR* const AgingTables[] =
	{
		TEXT("the US_*-backed cvar table (Private/Core/FPMUserSettingTable.g.h) - clause 6's denylist"),
		// ⚠ NOT STALE - DOES NOT EXIST YET. This is the governor's anti-ratchet baseline, and the
		// governor has not been ported to FPM2 (Private/ has no Governor at all, checked 2026-08-14).
		// Listed so it is DERIVED against whatever CL is current when the governor lands - not so the
		// watch keeps warning about a file nobody has written. Ant ruled this 2026-08-14; before that
		// the constant sat held at 495413 waiting on a table that was never going to arrive.
		TEXT("the vanilla-defaults snapshot - NOT YET BUILT; arrives with the governor, derive it then"),
		TEXT("the scalability group expansions (sg.* -> member cvars)"),
	};

	/** One read of our own descriptor, from DISK, because that is what §7.1 asks for. */
	struct FSelfDescriptor
	{
		bool    bRead = false;
		FString SemVersion;
		FString VersionName;
		FString RemoteVersionRange;
		FString RequiredOnRemote;   // string, not bool: "absent" and "false" are different findings
		FString DescriptorPath;
		FString ReadError;
	};

	const FSelfDescriptor& GetSelfDescriptor()
	{
		/*
		 * Read ONCE and cached. Not for speed — this runs on demand — but because a support bundle that
		 * re-read the file could report a DIFFERENT version than the one actually loaded, if someone
		 * swapped the file mid-session. The first read happens closest to load, so it is the honest one.
		 */
		static FSelfDescriptor Cached = []()
		{
			FSelfDescriptor D;

			const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager"));
			if (!Self.IsValid())
			{
				D.ReadError = TEXT("IPluginManager could not find our own plugin");
				return D;
			}

			D.DescriptorPath = Self->GetDescriptorFileName();
			D.VersionName    = Self->GetDescriptor().VersionName;

			/*
			 * ⚠ READ THE FILE, do not trust FPluginDescriptor for these three fields. `SemVersion`,
			 * `RemoteVersionRange` and `RequiredOnRemote` are SML's OWN keys, not Unreal's, so UE's
			 * parsed descriptor does not carry them. Reading the raw JSON is the only way to report what
			 * SML will actually act on — and reporting the value SML uses is the entire point of §7.1.
			 */
			FString Raw;
			if (!FFileHelper::LoadFileToString(Raw, *D.DescriptorPath))
			{
				D.ReadError = FString::Printf(TEXT("could not read %s"), *D.DescriptorPath);
				return D;
			}

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				D.ReadError = TEXT("descriptor is not valid JSON");
				return D;
			}

			Root->TryGetStringField(TEXT("SemVersion"),         D.SemVersion);
			Root->TryGetStringField(TEXT("RemoteVersionRange"), D.RemoteVersionRange);

			// RequiredOnRemote may be absent, bool, or (wrongly) a string. Report which, do not normalise.
			if (const TSharedPtr<FJsonValue> V = Root->TryGetField(TEXT("RequiredOnRemote")))
			{
				bool bVal = false;
				FString SVal;
				if (V->TryGetBool(bVal))        { D.RequiredOnRemote = bVal ? TEXT("true") : TEXT("false"); }
				else if (V->TryGetString(SVal)) { D.RequiredOnRemote = FString::Printf(TEXT("\"%s\" (STRING, expected bool)"), *SVal); }
				else                            { D.RequiredOnRemote = TEXT("<present but unreadable>"); }
			}
			else
			{
				D.RequiredOnRemote = TEXT("<absent from the descriptor>");
			}

			D.bRead = true;
			return D;
		}();

		return Cached;
	}

	const TCHAR* SideName(EFPMFixSide Side)
	{
		switch (Side)
		{
		case EFPMFixSide::Any:                     return TEXT("any");
		case EFPMFixSide::NeverOnDedicatedServer:  return TEXT("never-on-dedicated-server");
		default:                                   return TEXT("<unclassified>");
		}
	}

	/** True when the running game is not the build our tables were derived from. */
	bool GetChangelistDrift(uint32& OutRunning)
	{
		OutRunning = FEngineVersion::Current().GetChangelist();
		return OutRunning != DerivedFromChangelist;
	}
}

/**
 * ★ §7.10 + §7.12, run once at boot.
 *
 * Self-registers on `OnFEngineLoopInitComplete` rather than being called from `StartupModule`. Two
 * reasons: the module file does not grow a dependency on every diagnostic that wants boot time (the
 * coupling the old mod's structure died of, per the fix-contract's own note), and at StartupModule the
 * plugin manager is not reliably ready to hand us our own descriptor.
 */
static FDelayedAutoRegisterHelper GFPMSupportBootChecks(
	EDelayedRegisterRunPhase::EndOfEngineInit,
	[]()
	{
		const FSelfDescriptor& D = GetSelfDescriptor();

		// §7.12 — the parity self-check. On a join refusal, the log then NAMES the mismatch instead of
		// refusing mysteriously. Cheap, and it is the first thing anyone asks for.
		if (D.bRead)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] parity: SemVersion=%s  VersionName=%s  RemoteVersionRange=%s  RequiredOnRemote=%s"),
				*D.SemVersion, *D.VersionName, *D.RemoteVersionRange, *D.RequiredOnRemote);
		}
		else
		{
			// A failed self-read is itself the finding, and it must not be silent.
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] parity: COULD NOT READ OUR OWN DESCRIPTOR (%s). Remedy: confirm the .uplugin "
				     "shipped alongside the binary; SML's version gate reads the same file."), *D.ReadError);
		}

		// §7.10 — the drift watch.
		uint32 Running = 0;
		if (!GetChangelistDrift(Running))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] CL drift watch: game CL %u matches the CL our tables were derived from. Tables trusted."),
				Running);
			return;
		}

		/*
		 * WARN, with a remedy, naming every table that is now unverified — the WARN-remedy rule P1.1
		 * applies to existing warns. A bare "version mismatch" would be noise; the list is what makes it
		 * actionable, because it says exactly which artefacts need re-deriving.
		 */
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] CL DRIFT: running CL %u, but our tables were derived from CL %u. "
			     "The following are now UNVERIFIED against this build:"), Running, DerivedFromChangelist);
		for (const TCHAR* Table : AgingTables)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM]   - %s"), Table);
		}
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM]   Remedy: re-run 40-TOOLS/satisfactory/extract_user_settings.ps1 against the new "
			     "build's assets and re-check the vanilla defaults before trusting any lever."));

		/*
		 * AND TO CHAT. Ant, 2026-08-09: "a player wont check logs for stuff, only devs do." A game update
		 * that silently invalidates our tables is exactly the class of thing a player must be told about,
		 * because the symptom is a setting quietly doing the wrong thing. One line, not the whole list —
		 * "print what is relevant, not the entire log".
		 */
		FPMChatf(TEXT("[FPM] This game build (CL %u) is newer than the one FPM was tuned against (CL %u). "
		              "Some settings are unverified until FPM is updated."), Running, DerivedFromChangelist);
	});

/**
 * ★ §7.1 — the one-command support bundle.
 *
 * `FAutoConsoleCommandWithOutputDevice` deliberately: `Display`-level logs do NOT echo to the in-game
 * console, so a command whose output only reaches the log is a command that looks dead. Writing to the
 * output device puts the block where it can be read and copied without leaving the game.
 */
static FAutoConsoleCommandWithOutputDevice GFPMSupportCmd(
	TEXT("FPM.Support"),
	TEXT("Print one copyable support block: version, side, armed fixes, hooks, cvar ledger, diagnostics."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		const FSelfDescriptor& D = GetSelfDescriptor();

		Ar.Log(TEXT("================ FPM SUPPORT BUNDLE ================"));
		Ar.Log(TEXT("Copy everything between these rules when reporting a problem."));

		// ---- identity, read from disk ----
		if (D.bRead)
		{
			Ar.Logf(TEXT("version      : SemVersion=%s  VersionName=%s"), *D.SemVersion, *D.VersionName);
			Ar.Logf(TEXT("remote       : RemoteVersionRange=%s  RequiredOnRemote=%s"),
				*D.RemoteVersionRange, *D.RequiredOnRemote);
			Ar.Logf(TEXT("descriptor   : %s"), *D.DescriptorPath);
		}
		else
		{
			Ar.Logf(TEXT("version      : UNREADABLE (%s)  <- this is itself a finding"), *D.ReadError);
		}

		// ---- which side of the wire we are ----
		const bool bDedi = IsRunningDedicatedServer();
		Ar.Logf(TEXT("side         : %s"), bDedi ? TEXT("DEDICATED SERVER") : TEXT("client / listen host"));

		// ---- the build, and whether our tables still apply to it ----
		uint32 Running = 0;
		const bool bDrift = GetChangelistDrift(Running);
		Ar.Logf(TEXT("build        : CL %u  (tables derived from CL %u)%s"),
			Running, DerivedFromChangelist,
			bDrift ? TEXT("   *** DRIFT - tables unverified, see the boot WARN ***") : TEXT("   ok"));

		// ---- armed fixes, from the SAME list FPM.Diag.Dump reads ----
		const TArray<IFPMFix*>& Fixes = FPMFixes::Armed();
		Ar.Logf(TEXT("---- armed fixes: %d ----"), Fixes.Num());
		int32 Unnamed = 0;
		for (const IFPMFix* Fix : Fixes)
		{
			if (!Fix) { continue; }
			const bool bUnnamed = Fix->OriginStatus() == EFPMOriginStatus::ChokePointRepair
			                   || Fix->OriginStatus() == EFPMOriginStatus::UnknownCause;
			if (bUnnamed) { ++Unnamed; }

			Ar.Logf(TEXT("  %-28s side=%-26s diag=%-14s [%s]"),
				Fix->Name(), SideName(Fix->Side()), FPMDiag::NameOf(Fix->Channel()),
				LexToString(Fix->OriginStatus()));
		}
		/*
		 * The count of fixes still holding a symptom down without a named cause. Printed even when zero,
		 * because a number that is watched going down is worth more than a list nobody totals.
		 */
		Ar.Logf(TEXT("  fixes whose CAUSE IS NOT NAMED: %d of %d"), Unnamed, Fixes.Num());

		// ---- hook ledger ----
		const TArray<FPMHookRecord>& Hooks = FPMHookLedger::Records();
		Ar.Logf(TEXT("---- hooks: %d ----"), Hooks.Num());
		for (const FPMHookRecord& H : Hooks)
		{
			Ar.Logf(TEXT("  %-28s -> %-44s order=%-4d %s"),
				H.Owner  ? H.Owner  : TEXT("<null>"),
				H.Target ? H.Target : TEXT("<null>"),
				H.Order,
				H.bInstalled ? TEXT("installed") : TEXT("NOT INSTALLED"));
		}

		// ---- cvar ledger: the writer already knows how to print itself to a device ----
		Ar.Log(TEXT("---- cvar ledger ----"));
		FPMCVarWriter::Get().LogLedger(&Ar);

		/*
		 * ---- RHI MEMORY. This block exists because `stat rhi` DOES NOT EXIST in this build.
		 *
		 * Design §2's D0-client lists a `stat rhi` screenshot as U4 — "the single cheapest thing that
		 * turns the GPU strand into measurement". Ant tried it in-game 2026-08-09 and the console
		 * answered "Command not recognized: stat rhi"; `Stat D3D11RHI` likewise. The stat system is
		 * compiled out of Shipping, so U4 as written is undoable and no retry will fix it.
		 *
		 * RHIGetTextureMemoryStats returns the same numbers from code, and the texture-pool guard
		 * already links RHI for exactly this call — so FPM can just answer U4 itself. A protocol step
		 * that cannot be performed is worth replacing rather than leaving in the card for the next
		 * person to fail at.
		 */
		{
			FTextureMemoryStats Mem;
			RHIGetTextureMemoryStats(Mem);

			const auto MB = [](int64 Bytes) { return static_cast<int64>(Bytes / (1024 * 1024)); };

			Ar.Log(TEXT("---- RHI memory (stands in for 'stat rhi', which Shipping strips) ----"));
			Ar.Logf(TEXT("  dedicated video : %lld MB"), MB(Mem.DedicatedVideoMemory));
			Ar.Logf(TEXT("  dedicated system: %lld MB"), MB(Mem.DedicatedSystemMemory));
			Ar.Logf(TEXT("  shared system   : %lld MB"), MB(Mem.SharedSystemMemory));
			Ar.Logf(TEXT("  texture pool    : %lld MB   (this is what r.Streaming.PoolSize sets)"),
				MB(Mem.TexturePoolSize));
			// Field names read from RHIStats.h rather than guessed - my first attempt invented
			// AllocatedMemorySize, which does not exist on this struct.
			const int64 InPool = static_cast<int64>(Mem.StreamingMemorySize)
			                   + static_cast<int64>(Mem.NonStreamingMemorySize);
			Ar.Logf(TEXT("  in pool         : %lld MB   (streaming %lld MB, non-streaming %lld MB)"),
				MB(InPool), MB(static_cast<int64>(Mem.StreamingMemorySize)),
				MB(static_cast<int64>(Mem.NonStreamingMemorySize)));
			Ar.Logf(TEXT("  graphics mem    : %lld MB used of %lld MB total"),
				MB(Mem.UsedGraphicsMemory), MB(Mem.TotalGraphicsMemory));

			/*
			 * The number worth reading is the RATIO. Allocated approaching the pool size is the
			 * signature of a starved pool: measured on Ant's machine 2026-08-09, a 1000 MB pool on a
			 * 16303 MB card cost 57 FPS against 92, with the GPU idling at 83% waiting for textures.
			 */
			if (Mem.TexturePoolSize > 0)
			{
				const double Occupancy = static_cast<double>(InPool)
				                       / static_cast<double>(Mem.TexturePoolSize);
				Ar.Logf(TEXT("  pool occupancy  : %.0f%%%s"), Occupancy * 100.0,
					Occupancy > 0.90 ? TEXT("   *** OVER 90% - the pool is under pressure ***") : TEXT(""));
			}
		}

		// ---- diagnostics state ----
		Ar.Logf(TEXT("---- diagnostics ---- (silenced=%s)"),
			FPMDiag::IsSilenced() ? TEXT("YES") : TEXT("no"));

		/*
		 * ⚠ WHAT THIS BUNDLE DOES NOT YET CARRY, STATED SO NOBODY READS ITS ABSENCE AS "NOTHING WRONG".
		 * §7.1 also asks for "the named-exception keys, and the last N anomalies". Neither store exists in
		 * FPM2 yet: there is no anomaly ring buffer to read, and the recorded exceptions currently live in
		 * source comments rather than in a queryable registry. Saying so here is the difference between a
		 * bundle that is incomplete and a bundle that is quietly lying.
		 */
		Ar.Log(TEXT("---- not yet captured ----"));
		Ar.Log(TEXT("  last-N anomalies : NO RING BUFFER EXISTS YET (§7.1, outstanding)"));
		Ar.Log(TEXT("  exception keys   : recorded in source comments, not yet a queryable registry"));

		Ar.Log(TEXT("================ END SUPPORT BUNDLE ================"));
	}));
