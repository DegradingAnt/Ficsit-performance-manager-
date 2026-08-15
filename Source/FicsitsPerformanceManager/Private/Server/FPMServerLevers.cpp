// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Server/FPMServerLevers.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "Core/FPMUserSettingMap.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	bool bGFPMServerLeversReported = false;

	/**
	 * The audit body, WITHOUT the console echo.
	 *
	 * ★ THE SPLIT EXISTS BECAUSE THE ECHO CANNOT BE GIVEN GLog. `FPMScopedConsoleEcho` works by
	 * calling `GLog->AddOutputDevice(Ar)` (FPMConsoleEcho.cpp:17), so handing it `*GLog` would
	 * register the process-wide redirector INTO ITSELF and every subsequent log line would recurse.
	 * The automatic server run has no console device to echo to, so it calls this directly; only
	 * the console command, which has a real device, wraps it in the echo.
	 */
	void ReportInternal(bool bRequestedByHand);

	/**
	 * WHAT A CANDIDATE LEVER CAN TURN OUT TO BE.
	 *
	 * The order is the escalation order: Absent beats every other verdict because a name that is not
	 * here cannot be anything else, and LawRefused beats Settable because FPM's own rule outranks what
	 * the engine would permit.
	 */
	enum class EVerdict : uint8
	{
		Absent,        // no such console variable on THIS build
		ReadOnly,      // present, and the engine refuses a runtime write
		LawRefused,    // present and writable, but it is US_* backed - FPM hard rule forbids the write
		Settable,      // present, writable, not US_* backed
		Unknown,       // the law classifier failed its self-test, so Settable cannot be claimed
	};

	const TCHAR* VerdictName(const EVerdict V)
	{
		switch (V)
		{
		case EVerdict::Absent:     return TEXT("ABSENT");
		case EVerdict::ReadOnly:   return TEXT("READ-ONLY");
		case EVerdict::LawRefused: return TEXT("REFUSED BY FPM LAW (US_* backed)");
		case EVerdict::Settable:   return TEXT("settable");
		default:                   return TEXT("UNKNOWN (law classifier dead)");
		}
	}

	struct FLever
	{
		const TCHAR* Name;
		const TCHAR* Why;
	};

	struct FFamily
	{
		const TCHAR* Label;
		const TCHAR* Provenance;
		TArrayView<const FLever> Levers;
	};

	// L1 - the save cadence pair. RESEARCH-R41-SERVER-SIDE-2026-08-08.md:234 calls this the BEST
	// CANDIDATE and the number one thing to boot-test, against a measured 0.547-0.587 s game-thread
	// stall that every connected client feels at once.
	const FLever GSaveLevers[] =
	{
		{ TEXT("FG.SaveSession.EnableParallelSerialize"),  TEXT("moves save serialisation off the sole game thread") },
		{ TEXT("FG.SaveSession.ParallelSerializeChunkSize"), TEXT("work size per parallel chunk") },
	};

	// GC cadence. The one pacing lever the GC meter's own header names, plus the idle-server multiplier
	// that makes a dedicated server a different measurement from a client.
	//
	// gc.AllowIncrementalReachability is DELIBERATELY NOT HERE. It is a banked dead end - it crashed
	// the game in 34 seconds on Ant's save - and listing it as a candidate is how a dead end gets
	// re-proposed by the next reader. FPMGCMeter.h records the finding.
	const FLever GGcLevers[] =
	{
		{ TEXT("gc.TimeBetweenPurgingPendingKillObjects"), TEXT("stretches TIMER-driven passes only; forced ones are untouched") },
		{ TEXT("gc.TimeBetweenPurgingPendingKillObjectsOnIdleServerMultiplier"), TEXT("why an idle server already collects 10x slower") },
		{ TEXT("gc.MaxObjectsNotConsideredByGC"), TEXT("cluster/root set size") },
		{ TEXT("gc.NumRetriesBeforeForcingGC"), TEXT("how hard the engine tries before forcing a pass") },
	};

	// L3 - FG.Lightweight. The four per-frame budgets are the governor-shaped ones: they cap work per
	// frame, which is exactly what a dynamic governor modulates. The send-side pair is
	// server-authoritative. Names from the vanilla cvar TSV, harvested on a CLIENT - which is precisely
	// why their presence on a SERVER build is a question and not a given.
	const FLever GLightweightLevers[] =
	{
		{ TEXT("FG.Lightweight.MaxCustomizationUpdatesPerFrame"), TEXT("per-frame budget") },
		{ TEXT("FG.Lightweight.MaxNumTemporarySpawnsPerFrame"),   TEXT("per-frame budget") },
		{ TEXT("FG.Lightweight.MaxNumTemporaryDeletionsPerFrame"), TEXT("per-frame budget") },
		{ TEXT("FG.Lightweight.MaxNumConstructDataPerSend"),      TEXT("send-side, server-authoritative") },
		{ TEXT("FG.Lightweight.MaxNumReliableSends"),             TEXT("send-side, server-authoritative") },
		{ TEXT("FG.Lightweight.TimeBetweenCullTemporaries"),      TEXT("temporary lifetime") },
		{ TEXT("FG.Lightweight.UseBuildablePool"),                TEXT("actor recycling on/off") },
		{ TEXT("FG.Lightweight.PreallocatePoolSize"),             TEXT("pool sizing") },
		{ TEXT("FG.Lightweight.GrowPoolSize"),                    TEXT("pool sizing") },
	};

	// L5 - net.*. The last two families are FactoryGame's OWN additions rather than Epic's, and the
	// research names them as the most likely of the set to matter here.
	//
	// With two players on her server the per-connection scaling knobs are almost certainly irrelevant.
	// They are audited anyway because the cost of asking is one map lookup and the cost of NOT asking
	// is a governor designed around a name that does not exist.
	const FLever GNetLevers[] =
	{
		{ TEXT("net.RepresentationManager.NetFrequency"),               TEXT("CSS addition") },
		{ TEXT("net.RepresentationManager.MaxUpdatesPerUpdate"),        TEXT("CSS addition, per-frame budget") },
		{ TEXT("net.RepresentationManager.MaxPositionUpdatesPerUpdate"), TEXT("CSS addition, per-frame budget") },
		{ TEXT("net.RepresentationManager.MaxRemovalsPerUpdate"),       TEXT("CSS addition, per-frame budget") },
		{ TEXT("net.RepresentationManager.MovingUpdateInterval"),       TEXT("CSS addition") },
		{ TEXT("net.FGRepDetailCleanupFrequency"),                      TEXT("CSS addition") },
		{ TEXT("net.MaxConnectionsToTickPerServerFrame"),               TEXT("per-connection scaling") },
		{ TEXT("net.DormancyEnable"),                                   TEXT("dormancy") },
		{ TEXT("net.DormancyHysteresis"),                               TEXT("dormancy") },
		{ TEXT("net.UseAdaptiveNetUpdateFrequency"),                    TEXT("adaptive update rate") },
		{ TEXT("net.ProcessQueuedBunchesMillisecondLimit"),             TEXT("per-frame bunch budget") },
		{ TEXT("net.MaxRPCPerNetUpdate"),                               TEXT("RPC budget") },
		{ TEXT("net.ActorChannelPool"),                                 TEXT("channel recycling") },
	};

	// NetworkQuality, listed apart because its answer is already known and it is the one that would
	// have bitten. FG.NetworkQuality is US_NetworkQuality-backed (FPMUserSettingTable.g.h:54), so the
	// hard rule refuses it whatever the engine says. Auditing it is how the report DEMONSTRATES the
	// law check rather than merely asserting it.
	const FLever GQualityLevers[] =
	{
		{ TEXT("FG.NetworkQuality"), TEXT("vanilla's own ladder - US_* backed, expected REFUSED") },
	};

	const FFamily GFamilies[] =
	{
		{ TEXT("L1 save cadence"),  TEXT("R41:234, 'BEST CANDIDATE', runtime-settable UNVERIFIED"), GSaveLevers },
		{ TEXT("GC cadence"),       TEXT("FPMGCMeter.h - the one pacing lever, timer passes only"),  GGcLevers },
		{ TEXT("L3 FG.Lightweight"), TEXT("R41:264, 16 names, 'unverified for every one of them'"), GLightweightLevers },
		{ TEXT("L5 net.*"),         TEXT("R41:302, 'runtime-settable / defaults / sizes: all unmeasured'"), GNetLevers },
		{ TEXT("NetworkQuality"),   TEXT("US_* backed - the law check's own worked example"),        GQualityLevers },
	};

	/**
	 * A name that no console variable can carry, used as the presence classifier's known-negative.
	 *
	 * It is deliberately NOT in FPM's own cvar namespace: an `FPM.` prefix would be a name somebody
	 * could one day legitimately register, and the day they did, this self-test would start passing for
	 * the wrong reason and stop discriminating. The dots and the sentence make it unregisterable in
	 * practice and unmistakable in intent.
	 */
	const TCHAR* KnownAbsentProbeName()
	{
		return TEXT("fpm.server.levers.this.console.variable.must.never.exist");
	}

	/**
	 * ★ THE PRESENCE CLASSIFIER'S LIVENESS PROOF - the same shape as
	 * FPMSaveSettingsInterceptor.cpp:63, for the same reason.
	 *
	 * Without it, "every lever is ABSENT" and "the lookup is broken" print IDENTICALLY, and the second
	 * one is far more likely than it sounds: a console manager queried before its objects register,
	 * a build where the whole family moved namespace, a typo in a shared prefix. The report would then
	 * say "this build has none of the governor's levers", which reads as a finding, gets believed, and
	 * quietly kills a lever family that was there all along. A confident wrong answer, not a missing
	 * one - which is the failure this project keeps paying for.
	 *
	 * A check that merely calls FindConsoleVariable and expects no crash proves nothing. The thing in
	 * doubt is whether it can still tell the two cases APART, so it needs both ends:
	 *   - known-positive: FPMCVarWriter's own scratch probe, registered by this module, so its presence
	 *     is guaranteed by our own code rather than by an engine version.
	 *   - known-negative: a name that cannot exist.
	 */
	bool FPMServerLeverPresenceIsAlive()
	{
		IConsoleManager& Console = IConsoleManager::Get();
		const bool bPositive = Console.FindConsoleVariable(FPMCVarWriter::SelfTestProbeName(), false) != nullptr;
		const bool bNegative = Console.FindConsoleVariable(KnownAbsentProbeName(), false) != nullptr;
		if (bPositive && !bNegative) { return true; }

		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ THE LEVER LOOKUP IS DEAD: '%s' was %s (expected FOUND) and the known-absent "
			     "probe was %s (expected NOT found). FindConsoleVariable cannot discriminate here, so "
			     "every ABSENT below would be a statement about the instrument and not about this build. "
			     "REFUSING to print the audit rather than publishing a table of confident zeros."),
			FPMCVarWriter::SelfTestProbeName(),
			bPositive ? TEXT("found") : TEXT("NOT FOUND"),
			bNegative ? TEXT("FOUND") : TEXT("not found"));
		return false;
	}

	/**
	 * ★ THE LAW CLASSIFIER'S LIVENESS PROOF. Same two probes the save-settings interceptor and the
	 * residue sentinel already use, because the classifier is literally the same one.
	 *
	 * Its failure is the more dangerous of the two: a dead IsBacked answers "not backed" for
	 * everything, so a US_*-backed cvar would be reported `settable`, a future governor author would
	 * read that as permission, and the write would land in GameUserSettings.ini permanently. So a
	 * failure here does not merely warn - it downgrades every settable verdict to UNKNOWN, and the
	 * report says why.
	 */
	bool FPMServerLeverLawIsAlive()
	{
		const bool bPositive = FPMUserSettingMap::IsBacked(TEXT("t.MaxFPS"));
		const bool bNegative = FPMUserSettingMap::IsBacked(FPMCVarWriter::SelfTestProbeName());
		if (bPositive && !bNegative) { return true; }

		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ THE US_* LAW CHECK IS DEAD: 't.MaxFPS' classified %s (expected backed) and "
			     "'%s' classified %s (expected NOT backed). Every 'settable' below is therefore "
			     "downgraded to UNKNOWN - a US_*-backed cvar misreported as settable is how a governor "
			     "write ends up serialised into GameUserSettings.ini for good."),
			bPositive ? TEXT("backed") : TEXT("NOT backed"),
			FPMCVarWriter::SelfTestProbeName(),
			bNegative ? TEXT("BACKED") : TEXT("not backed"));
		return false;
	}

	struct FReading
	{
		EVerdict Verdict = EVerdict::Absent;
		FString Value;
		FString SetBy;
		bool bCheat = false;
	};

	FReading Read(const TCHAR* Name, const bool bLawAlive)
	{
		FReading Out;

		// bTrackFrequentCalls = false ON PURPOSE. A failed find bumps an engine counter that warns at
		// 30 misses per name and at 500 overall (ConsoleManager.cpp:2328,2334). This audit EXPECTS
		// misses - they are its findings - and a diagnostic that manufactures engine warnings to
		// report them would be creating the noise the project's own rule says to fix at the cause.
		// The counter is compiled out in Shipping anyway (IConsoleManager.h:17), so passing false is
		// what makes the Development build behave like the shipping one instead of differently.
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name, false);
		if (!Var)
		{
			Out.Verdict = EVerdict::Absent;
			return Out;
		}

		Out.Value = Var->GetString();
		const EConsoleVariableFlags Flags = Var->GetFlags();
		Out.SetBy = GetConsoleVariableSetByName(static_cast<EConsoleVariableFlags>(Flags & ECVF_SetByMask));
		Out.bCheat = (Flags & ECVF_Cheat) != 0;

		if ((Flags & ECVF_ReadOnly) != 0)
		{
			Out.Verdict = EVerdict::ReadOnly;
		}
		else if (!bLawAlive)
		{
			Out.Verdict = EVerdict::Unknown;
		}
		else if (FPMUserSettingMap::IsBacked(Name))
		{
			Out.Verdict = EVerdict::LawRefused;
		}
		else
		{
			Out.Verdict = EVerdict::Settable;
		}
		return Out;
	}
}

FFPMServerLevers& FFPMServerLevers::Get()
{
	static FFPMServerLevers Instance;
	return Instance;
}

void FFPMServerLevers::Arm()
{
	// Armed everywhere; the AUTO-run is server-only. The console command below works on a client too,
	// deliberately - the client reading is the comparison baseline that turns "absent on the server"
	// into "absent on the server AND present on the client", which is a different and much more useful
	// finding than either half alone.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] server-lever audit armed. It reports automatically on the first world load of a "
		     "DEDICATED SERVER, and on demand anywhere. It writes nothing."));
}

void FFPMServerLevers::OnWorldLoad(UWorld* World)
{
	if (!IsRunningDedicatedServer())
	{
		// Said out loud rather than skipped silently, for the same reason FPMFixes::Arm logs its
		// dedicated-server gate: "did not run" and "ran and found nothing" must never look the same.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] server-lever audit: not a dedicated server, so the automatic run is skipped. "
			     "FPM.Server.Levers runs it here on demand."));
		return;
	}

	if (bGFPMServerLeversReported)
	{
		return;
	}
	bGFPMServerLeversReported = true;

	ReportInternal(false);
}

void FFPMServerLevers::Disarm()
{
	// Nothing is installed, so nothing is removed. The flag is reset so a re-arm audits again rather
	// than reporting a silent nothing - a disarmed-then-rearmed instrument that stays quiet is the
	// dead-instrument shape arriving through the back door.
	bGFPMServerLeversReported = false;
}

void FFPMServerLevers::ReportNow(FOutputDevice& Ar)
{
	FPMScopedConsoleEcho Echo(&Ar);
	ReportInternal(true);
}

namespace
{
void ReportInternal(const bool bRequestedByHand)
{
	// The channel's help text says "0 = silent", so 0 must actually silence it or the switch is a dead
	// control and its own description is a false claim - this project's named defect. A HAND-TYPED
	// FPM.Server.Levers still prints: somebody asked, and answering "no" to a direct question by
	// printing nothing is the worst reply available.
	if (!bRequestedByHand && !FPMDiag::IsOn(FPMDiag::EChannel::ServerLevers))
	{
		return;
	}

	if (!FPMServerLeverPresenceIsAlive())
	{
		// The self-test has already logged WHY at Error. Refusing here is the whole point: a table of
		// ABSENT rows produced by a broken lookup is worse than no table, because it reads like a
		// finding about the build.
		return;
	}

	const bool bLawAlive = FPMServerLeverLawIsAlive();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] server-lever audit - which governor levers EXIST here, and which accept a write. "
		     "This build is %s. Nothing below is written; this is a reading."),
		IsRunningDedicatedServer() ? TEXT("a DEDICATED SERVER") : TEXT("NOT a dedicated server (client/editor reading)"));

	int32 TotalAsked = 0;
	int32 TotalSettable = 0;
	int32 TotalAbsent = 0;

	for (const FFamily& Family : GFamilies)
	{
		int32 Settable = 0;
		int32 Absent = 0;
		int32 ReadOnly = 0;
		int32 Refused = 0;
		int32 Unknown = 0;

		for (const FLever& Lever : Family.Levers)
		{
			const FReading R = Read(Lever.Name, bLawAlive);
			switch (R.Verdict)
			{
			case EVerdict::Absent:     ++Absent;   break;
			case EVerdict::ReadOnly:   ++ReadOnly; break;
			case EVerdict::LawRefused: ++Refused;  break;
			case EVerdict::Settable:   ++Settable; break;
			default:                   ++Unknown;  break;
			}

			// Level 2 is the per-lever detail. It is a long table and it is the raw evidence, so it is
			// held back from the default level while the per-family verdict below is not.
			if (bRequestedByHand || FPMDiag::IsOn(FPMDiag::EChannel::ServerLevers, 2))
			{
				if (R.Verdict == EVerdict::Absent)
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM]     %-58s ABSENT        (%s)"), Lever.Name, Lever.Why);
				}
				else
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM]     %-58s %-14s = %s (set by %s)%s"),
						Lever.Name, VerdictName(R.Verdict), *R.Value, *R.SetBy,
						R.bCheat ? TEXT(" [ECVF_Cheat]") : TEXT(""));
				}
			}
		}

		const int32 Asked = Family.Levers.Num();
		TotalAsked += Asked;
		TotalSettable += Settable;
		TotalAbsent += Absent;

		// ★ THE LINE A GOVERNOR DESIGNER READS. A family at zero reachable is not a small result: it
		// means that whole branch of the design is built on names this binary does not have, and it
		// says so at Warning so it cannot be skimmed past.
		if (Settable == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   %s: 0 of %d usable (%d absent, %d read-only, %d refused by law, %d unknown). "
				     "NOTHING in this family is a lever on this build - %s"),
				Family.Label, Asked, Absent, ReadOnly, Refused, Unknown, Family.Provenance);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM]   %s: %d of %d usable (%d absent, %d read-only, %d refused by law, %d unknown). %s"),
				Family.Label, Settable, Asked, Absent, ReadOnly, Refused, Unknown, Family.Provenance);
		}
	}

	// ⚠ THE COVERAGE LINE. An instrument that reports a count without its denominator invites the
	// reader to treat the count as the whole world. This table is a FIXED list of names somebody wrote
	// down, so it is silent about every lever nobody thought of, and that limit belongs in the output
	// rather than in a design document nobody reads next to the log.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   coverage: %d named candidate(s) asked, %d settable, %d absent. This is the whole "
		     "question this instrument can answer - a lever nobody listed is not reported as missing, "
		     "it is not reported at all. Law check %s."),
		TotalAsked, TotalSettable, TotalAbsent,
		bLawAlive ? TEXT("live") : TEXT("DEAD - every 'settable' was downgraded to UNKNOWN"));

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ⚠ 'settable' means the ENGINE would accept a write. It is not a claim that moving "
		     "it helps, and not permission to move it: R41 §2.4 forbids any lever that changes "
		     "simulation OUTCOMES rather than cost, and that judgement is per lever, not per family."));
}

}   // namespace

static FAutoConsoleCommandWithOutputDevice GFPMServerLeversCmd(
	TEXT("FPM.Server.Levers"),
	TEXT("Server governor: which candidate levers exist on this build and which accept a write. Reads "
	     "only - writes no console variable, no ini, nothing."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMServerLevers::ReportNow(Ar);
	}));
