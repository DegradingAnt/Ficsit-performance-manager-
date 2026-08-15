// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMNetGuidCensus.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMDiag.h"

#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/World.h"

#include "HAL/IConsoleManager.h"

#include <atomic>

namespace
{
	/**
	 * Total SupportsObject rejections this session. Atomic because object-reference serialization is
	 * not guaranteed to be game-thread-only, and the RPC gate already paid for assuming otherwise.
	 */
	std::atomic<uint64> GNetGuidCensusRejected{0};

	/**
	 * First-sighting census, bounded so it cannot become the spam it exists to summarise.
	 *
	 * !! GAME THREAD ONLY. A TSet is not atomic. The counter above is; this is not, and the guard at
	 * the call site is what keeps two threads from racing Add() and corrupting the storage. Losing a
	 * census line on a worker costs one name. Losing the set costs the instrument.
	 */
	TSet<FName> GNetGuidCensusSeen;
	bool bGNetGuidCensusSaturated = false;

	/**
	 * The cap. The 2026-08-07 corpus produced exactly SIX distinct classes across 8,928 rejections
	 * (LightweightCollisionComponent, StaticMeshComponent, FGColoredInstanceMeshProxy, SceneComponent,
	 * InstancedStaticMeshComponent, Level), so sixteen is comfortable headroom over the measured shape
	 * while still bounding a stack that goes wrong. WHAT HAPPENS AT THE CAP: further rejections are
	 * still COUNTED, they are simply no longer NAMED, and the census says so once, at Warning level.
	 * A bounded list that goes quiet without saying so reads as a complete list.
	 */
	constexpr int32 GNetGuidCensusLimit = 16;

	/** Latched once, read-modify-write, so two racing workers cannot both print the off-thread note. */
	std::atomic<bool> bGNetGuidCensusOffThreadReported{false};

	/**
	 * The one net driver worth reporting on: the game one, reached through a world context rather than
	 * GWorld, because GWorld is not reliable outside the game thread and is not guaranteed to be the
	 * context that owns the connection.
	 *
	 * Returns nullptr in single player and in the main menu, and the caller MUST distinguish that from
	 * a zero. There is no net driver there at all, so every counter below would read 0 and a bare 0
	 * would say "no traffic" when the truth is "no network session".
	 */
	UNetDriver* FindGameNetDriver()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::Game && Context.WorldType != EWorldType::PIE)
			{
				continue;
			}
			if (const UWorld* World = Context.World())
			{
				if (UNetDriver* Driver = World->GetNetDriver())
				{
					return Driver;
				}
			}
		}
		return nullptr;
	}
}

FFPMNetGuidCensus& FFPMNetGuidCensus::Get()
{
	static FFPMNetGuidCensus Instance;
	return Instance;
}

void FFPMNetGuidCensus::Arm()
{
	/**
	 * AFTER, not BEFORE, and that is the whole design. The rejection is the RESULT, so a BEFORE hook
	 * would have to re-derive it by calling IsFullNameStableForNetworking and IsSupportedForNetworking
	 * itself, which is a second copy of the engine test that can silently disagree with the first.
	 * Reading the real answer cannot drift from it.
	 *
	 * Scalar bool return, so this takes SML ApplyCallScalar and never the by-value trampoline that
	 * rules out the InternalLoadObject half. See the header.
	 *
	 * The lambda is NAMED before it reaches the macro. Its parameter list holds TWeakObjectPtr<UObject>,
	 * and the preprocessor does not treat angle brackets as grouping, so passing it inline would split
	 * on a comma the moment anyone widened that template.
	 */
	auto OnSupportsObject = [](const bool& bSupported, const FNetGUIDCache* Cache, const UObject* Object,
	                           const TWeakObjectPtr<UObject>* WeakObjectPtr)
	{
		// Supported, or nothing to name. Both are the normal case and neither is a finding.
		if (bSupported || Object == nullptr)
		{
			return;
		}

		const uint64 Count = ++GNetGuidCensusRejected;

		if (!IsInGameThread())
		{
			/**
			 * Not an error in itself, unlike the RPC gate case which stood in front of an engine
			 * assertion. It is recorded because it decides whether the census below is COMPLETE: every
			 * off-thread rejection is counted but never named, so a reader comparing the total against
			 * the named classes needs to know that gap exists. Once per session.
			 */
			if (!bGNetGuidCensusOffThreadReported.exchange(true, std::memory_order_relaxed))
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] net-guid census: SupportsObject was called off the game thread. Those "
					     "rejections are counted but NOT named, so the class list is a subset of the "
					     "total. Reported once per session."));
			}
			return;
		}

		const FName ClassName = Object->GetClass()->GetFName();

		if (GNetGuidCensusSeen.Num() < GNetGuidCensusLimit)
		{
			if (!GNetGuidCensusSeen.Contains(ClassName))
			{
				GNetGuidCensusSeen.Add(ClassName);

				if (FPMDiag::IsOn(FPMDiag::EChannel::NetGuidCensus))
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM] net-guid census: %s cannot be net-addressed, so every reference to "
						     "one serialises as null and the engine logs a warning. First sighting."),
						*ClassName.ToString());
				}
			}
		}
		else if (!bGNetGuidCensusSaturated)
		{
			bGNetGuidCensusSaturated = true;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] net-guid census: class list FULL at %d. Further rejections are still "
				     "COUNTED but no longer NAMED. The list above is not the complete set."),
				GNetGuidCensusLimit);
		}

		if (FPMDiag::IsOn(FPMDiag::EChannel::NetGuidCensus)
			&& (Count == 1 || (Count % FPMLog::ThrottleRoutine) == 0))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] net-guid census: %llu unsupported object reference(s) this session"), Count);
		}
	};

	SupportsObjectHandle = FPM_SUBSCRIBE_AFTER("net-guid-census", FNetGUIDCache::SupportsObject, OnSupportsObject);
}

void FFPMNetGuidCensus::Disarm()
{
	/**
	 * Guarded on IsValid() because the editor path installs nothing and hands back an invalid handle,
	 * and RemoveHandler would then walk maps SML never allocated.
	 */
	if (SupportsObjectHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(FNetGUIDCache::SupportsObject, SupportsObjectHandle);
		SupportsObjectHandle.Reset();
	}
}

void FFPMNetGuidCensus::GetCounts(uint64& OutRejectedTotal, int32& OutSeenClasses, bool& OutCensusSaturated)
{
	OutRejectedTotal = GNetGuidCensusRejected.load();
	OutSeenClasses = GNetGuidCensusSeen.Num();
	OutCensusSaturated = bGNetGuidCensusSaturated;
}

void FFPMNetGuidCensus::LogReport(FOutputDevice* Ar)
{
	uint64 RejectedTotal = 0;
	int32 SeenClasses = 0;
	bool bSaturated = false;
	GetCounts(RejectedTotal, SeenClasses, bSaturated);

	TArray<FString> Lines;

	if (RejectedTotal == 0)
	{
		/**
		 * The two zeros that must not look alike. A fix that is OFF BY DEFAULT reporting 0 is almost
		 * always reporting that nobody armed it, and reading that as health is the failure this whole
		 * survey was written to avoid.
		 */
		Lines.Add(FString::Printf(
			TEXT("[FPM] net-guid census: 0 unsupported object references. This census is OFF BY DEFAULT, "
			     "so a zero here may mean it was never armed rather than that nothing was rejected. "
			     "Run FPM.Fix.NetGuidCensus 1, play for a minute near any lightweight buildable, then "
			     "ask again.")));
	}
	else
	{
		Lines.Add(FString::Printf(
			TEXT("[FPM] net-guid census: %llu unsupported object reference(s) from %d named class(es)%s."),
			RejectedTotal, SeenClasses,
			bSaturated
				? TEXT(", class list FULL so later offenders were counted but not named")
				: TEXT("")));
	}

	/**
	 * COVERAGE, PRINTED EVERY RUN AND NOT ONLY WHEN IT IS BAD. This census sees one of the two
	 * LogNetPackageMap families. Saying so beside the number is what stops the number being read as the
	 * whole picture, which is exactly how a partial instrument becomes a false all-clear.
	 */
	Lines.Add(FString::Printf(
		TEXT("[FPM] net-guid census COVERAGE: this counts FNetGUIDCache::SupportsObject rejections only "
		     "(8,928 of the 33,390 LogNetPackageMap warnings in the 2026-08-07 server corpus, i.e. 27%%). "
		     "The other 24,462, UPackageMapClient::InternalLoadObject unresolved client GUIDs, are NOT "
		     "counted here: that function returns FNetworkGUID by value and hooking it through SML would "
		     "mis-model the return ABI. Read the server log for that family.")));

	for (const FString& Line : Lines)
	{
		if (Ar != nullptr)
		{
			Ar->Log(Line);
		}
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
	}
}

/**
 * FPM.NetGuidCensus.Report. Takes the output device so it answers in the console she is looking at as
 * well as in the log, because a Display UE_LOG alone does not echo to the in-game console and a command
 * that answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMNetGuidCensusReportCmd(
	TEXT("FPM.NetGuidCensus.Report"),
	TEXT("Print how many object references were rejected as un-net-addressable this session, which "
	     "classes they were, and what share of the known warning traffic this census can see."),
	// ⚠ THE FRAME CAP AND THE FPM.Diag.NetGuidCensus CHANNEL ARE NOT THE SAME LEVER. That channel is
	// documented as never gating this command, because a bounded list going quiet must not be able to
	// look complete. The frame cap does not silence anything either: it refuses a SECOND report inside
	// one tick and says so on the console. See FPMConsoleEcho.h.
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMReportGate Gate(Ar, TEXT("FPM.NetGuidCensus.Report"));
		if (Gate.IsRefused())
		{
			return;
		}

		FFPMNetGuidCensus::LogReport(&Ar);
	}));

/**
 * ================================ FPM.Net.Report ================================
 *
 * A SEPARATE INSTRUMENT THAT SHARES THIS FILE AND NOTHING ELSE. It installs no hook, holds no state,
 * and does not care whether the census above is armed. It is registered at file scope for exactly that
 * reason: turning the census off must not take the byte counters away with it.
 *
 * IT READS ONLY THE UNCONDITIONAL ACCUMULATORS, AND THAT IS THE WHOLE POINT.
 * UNetDriver::InTotalBytes / OutTotalBytes / InTotalPackets / OutTotalPackets / InTotalBunches /
 * OutTotalBunches / OutTotalReliableBunches / InTotalPacketsLost / OutTotalPacketsLost are incremented
 * with no preprocessor guard and no cvar gate, in NetConnection.cpp:2091-2100 (inbound) and
 * :2451-2455 (outbound). They are FACTS in a Shipping build.
 *
 * !! WHAT IT DELIBERATELY DOES NOT READ, AND WHY. InBytesPerSecond and OutBytesPerSecond LOOK like the
 * numbers anyone actually wants, and they are gated: the whole derivation sits behind
 *   if (bCollectNetStats || bCollectServerStats || GbEnableNetStats)     NetDriver.cpp:8229
 * with a second gate at :8330, where GbEnableNetStats is the cvar net.EnableNetStats (NetDriver.cpp
 * 462-467, default false). Read them without setting it and they return a confident 0 that means
 * "nobody turned the counter on" while reading exactly like "no traffic". They are not read here, and
 * the output says so, because a silent omission is how the next reader re-adds them.
 *
 * !! AND WHAT ALSO CANNOT BE MEASURED HERE, checked rather than assumed. USE_NETWORK_PROFILER is
 * !(UE_BUILD_SHIPPING || UE_BUILD_TEST) at Build.h:368-369, and the Shipping STATS define at Build.h:333
 * is (FORCE_USE_STATS && !ENABLE_STATNAMEDEVENTS). So the -networkprofiler flag and stat net are both
 * compiled out of the game she plays, and per-class byte attribution is not available to any mod.
 *
 * !! THE COUNTERS ARE uint32 AND THEY WRAP, at 4 GiB and at 4 billion packets since the driver was
 * created. The output says so rather than presenting a wrapped total as a measurement.
 */
static void FPMNetReport(FOutputDevice* Ar)
{
	TArray<FString> Lines;

	UNetDriver* Driver = FindGameNetDriver();

	if (Driver == nullptr)
	{
		/** Not a zero. There is no net driver, so there is nothing that COULD have counted. */
		Lines.Add(FString(
			TEXT("[FPM] net report: NO NET DRIVER. This is single player, the main menu, or a session "
			     "that has not joined yet. Nothing was measured and nothing was zero.")));
	}
	else
	{
		const bool bIsClient = Driver->ServerConnection != nullptr;
		const int32 ClientCount = Driver->ClientConnections.Num();

		Lines.Add(FString::Printf(
			TEXT("[FPM] net report: %s, driver %s, %d client connection(s)."),
			bIsClient ? TEXT("CLIENT") : TEXT("SERVER or LISTEN HOST"),
			*Driver->NetDriverName.ToString(),
			ClientCount));

		Lines.Add(FString::Printf(
			TEXT("[FPM] net report: bytes  in %.2f MiB / out %.2f MiB   (raw in %u / out %u)"),
			static_cast<double>(Driver->InTotalBytes) / (1024.0 * 1024.0),
			static_cast<double>(Driver->OutTotalBytes) / (1024.0 * 1024.0),
			Driver->InTotalBytes, Driver->OutTotalBytes));

		Lines.Add(FString::Printf(
			TEXT("[FPM] net report: packets in %u / out %u, lost in %u / out %u"),
			Driver->InTotalPackets, Driver->OutTotalPackets,
			Driver->InTotalPacketsLost, Driver->OutTotalPacketsLost));

		Lines.Add(FString::Printf(
			TEXT("[FPM] net report: bunches in %u / out %u, of which %u outgoing were reliable"),
			Driver->InTotalBunches, Driver->OutTotalBunches, Driver->OutTotalReliableBunches));

		Lines.Add(FString(
			TEXT("[FPM] net report: totals are since the driver was created, and they are uint32 - they "
			     "WRAP at 4 GiB. A long session can roll them over, so treat a fall between two readings "
			     "as a wrap rather than as negative traffic.")));
	}

	/** Printed whether or not a driver was found, because the limits of the instrument do not depend
	 *  on whether it had anything to read. */
	Lines.Add(FString(
		TEXT("[FPM] net report LIMITS: per-second rates are NOT shown. InBytesPerSecond and "
		     "OutBytesPerSecond are gated behind net.EnableNetStats and would read 0 without it, which "
		     "is indistinguishable from no traffic. Per-class byte attribution is impossible in this "
		     "build: the engine network profiler and stat net are both compiled out of Shipping.")));

	for (const FString& Line : Lines)
	{
		if (Ar != nullptr)
		{
			Ar->Log(Line);
		}
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
	}
}

static FAutoConsoleCommandWithOutputDevice GFPMNetReportCmd(
	TEXT("FPM.Net.Report"),
	TEXT("Print total bytes, packets and bunches this session from the engine own unconditional "
	     "counters, plus what this instrument cannot see. Reads only; installs nothing."),
	// ⚠ THIS COMMAND SHARES ONE FRAME CLAIM WITH THE CENSUS REPORT ABOVE AND WITH ALL TEN OTHERS, so
	// running both in one tick refuses the second. FPMConsoleEcho.h carries why that is the right trade.
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMReportGate Gate(Ar, TEXT("FPM.Net.Report"));
		if (Gate.IsRefused())
		{
			return;
		}

		FPMNetReport(&Ar);
	}));
