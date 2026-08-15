// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMPowerWarningProbe.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMOverlay.h"
#include "UI/FPMChatRelay.h"

#include "FGCircuitSubsystem.h"
#include "FGPowerCircuit.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/** One reading. Every field is here so a later line can be compared against an earlier one. */
	struct FFPMPowerSample
	{
		int32 MapCircuits = 0;        // mCircuits, the authoritative TMap. Empty on a client by design.
		int32 ReplCircuits = 0;       // mReplicatedCircuits, the parallel array that DOES replicate.
		int32 PowerCircuits = 0;      // of those, the ones that are actually UFGPowerCircuit
		int32 FuseTriggered = 0;      // ★ the number the whole probe exists to produce
		bool bSubsystem = false;      // false means we could not even ask
	};

	/*
	 * SAMPLE FOR A MINUTE, NOT ONCE. mIsFuseTriggered is ReplicatedUsing, so a joining client's copy
	 * arrives AFTER the join. A single reading at world load measures the replication delay.
	 *
	 * Two minutes would be tidier and is not free: this walks every circuit each time, and a large save
	 * has hundreds. Sixty seconds at two-second intervals is 30 walks, which is nothing next to a load
	 * screen and long enough that the popup (reported at login) has certainly fired.
	 */
	constexpr float GFPMPowerSampleIntervalSeconds = 2.0f;
	constexpr float GFPMPowerWindowSeconds = 60.0f;

	FTSTicker::FDelegateHandle GFPMPowerTicker;
	TWeakObjectPtr<UWorld> GFPMPowerWorld;
	float GFPMPowerElapsed = 0.0f;
	FFPMPowerSample GFPMPowerLast;
	bool GFPMPowerHaveLast = false;
	int32 GFPMPowerPeakTriggered = 0;

	/** Thin wrapper so the rest of this file reads in samples rather than out-params. */
	FFPMPowerSample Sample(UWorld* World)
	{
		FFPMPowerSample S;
		S.bSubsystem = FFPMPowerWarningProbe::ReadCircuitCounts(
			World, S.MapCircuits, S.ReplCircuits, S.PowerCircuits, S.FuseTriggered);
		return S;
	}

	FString Describe(const FFPMPowerSample& S)
	{
		if (!S.bSubsystem)
		{
			return TEXT("no circuit subsystem yet - CANNOT ANSWER, which is not the same as 'nothing tripped'");
		}
		if (S.PowerCircuits == 0)
		{
			return FString::Printf(
				TEXT("0 power circuits visible (map %d, replicated %d) - CANNOT ANSWER YET"),
				S.MapCircuits, S.ReplCircuits);
		}
		return FString::Printf(
			TEXT("%d of %d power circuit(s) report a TRIGGERED FUSE  [map %d, replicated %d]"),
			S.FuseTriggered, S.PowerCircuits, S.MapCircuits, S.ReplCircuits);
	}

	/** Same shape as a sample for change-detection purposes. Counts only; pointers are not compared. */
	bool SameAs(const FFPMPowerSample& A, const FFPMPowerSample& B)
	{
		return A.bSubsystem == B.bSubsystem && A.PowerCircuits == B.PowerCircuits
			&& A.FuseTriggered == B.FuseTriggered && A.MapCircuits == B.MapCircuits
			&& A.ReplCircuits == B.ReplCircuits;
	}

	void StopSampler()
	{
		if (GFPMPowerTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(GFPMPowerTicker);
			GFPMPowerTicker.Reset();
		}
	}
}

FFPMPowerWarningProbe& FFPMPowerWarningProbe::Get()
{
	static FFPMPowerWarningProbe Instance;
	return Instance;
}

bool FFPMPowerWarningProbe::ReadCircuitCounts(UWorld* World, int32& OutMapCircuits,
	int32& OutReplicatedCircuits, int32& OutPowerCircuits, int32& OutFuseTriggered)
{
	OutMapCircuits = OutReplicatedCircuits = OutPowerCircuits = OutFuseTriggered = 0;
	if (World == nullptr) { return false; }

	AFGCircuitSubsystem* Subsystem = AFGCircuitSubsystem::Get(World);
	if (Subsystem == nullptr) { return false; }

	/*
	 * BOTH CONTAINERS, DEDUPLICATED BY POINTER. In single player and on a listen host the same circuit
	 * is in both, and counting it twice would inflate every number here. On a joining client the map is
	 * empty and only the array has anything — the exact case a map-only read would have reported as "no
	 * circuits at all", on the one machine where the popup is being complained about.
	 */
	TSet<const UFGCircuit*> Seen;

	auto Consider = [&](UFGCircuit* Circuit)
	{
		if (Circuit == nullptr || Seen.Contains(Circuit)) { return; }
		Seen.Add(Circuit);

		if (UFGPowerCircuit* Power = Cast<UFGPowerCircuit>(Circuit))
		{
			++OutPowerCircuits;
			if (Power->IsFuseTriggered()) { ++OutFuseTriggered; }
		}
	};

	for (const TPair<int32, TObjectPtr<UFGCircuit>>& Pair : Subsystem->mCircuits)
	{
		++OutMapCircuits;
		Consider(Pair.Value);
	}
	for (const TObjectPtr<UFGCircuit>& Circuit : Subsystem->mReplicatedCircuits)
	{
		++OutReplicatedCircuits;
		Consider(Circuit);
	}

	return true;
}

bool FFPMPowerWarningProbe::EnumerateTrippedCircuits(UWorld* World, int32& OutExamined,
	int32& OutUnreadable, TArray<int32>& OutTrippedCircuitIDs)
{
	OutExamined = OutUnreadable = 0;
	OutTrippedCircuitIDs.Reset();
	if (World == nullptr) { return false; }

	AFGCircuitSubsystem* Subsystem = AFGCircuitSubsystem::Get(World);
	if (Subsystem == nullptr) { return false; }

	// Same dedup-by-pointer reasoning as ReadCircuitCounts above — see that function's comment.
	TSet<const UFGCircuit*> Seen;

	auto Consider = [&](UFGCircuit* Circuit)
	{
		if (Circuit == nullptr)
		{
			++OutUnreadable;   // a null slot in the container — present, but nothing to read
			return;
		}
		if (Seen.Contains(Circuit)) { return; }
		Seen.Add(Circuit);

		if (UFGPowerCircuit* Power = Cast<UFGPowerCircuit>(Circuit))
		{
			++OutExamined;
			if (Power->IsFuseTriggered())
			{
				OutTrippedCircuitIDs.Add(Power->GetCircuitID());
			}
		}
		else
		{
			// A UFGCircuit that is not a UFGPowerCircuit. Nothing else derives from UFGCircuit in this
			// codebase today (checked against FGPowerCircuit.h) so this branch should be unreachable —
			// counted as unreadable rather than assumed away, so a future circuit type does not vanish
			// from the coverage numbers.
			++OutUnreadable;
		}
	};

	for (const TPair<int32, TObjectPtr<UFGCircuit>>& Pair : Subsystem->mCircuits)
	{
		Consider(Pair.Value);
	}
	for (const TObjectPtr<UFGCircuit>& Circuit : Subsystem->mReplicatedCircuits)
	{
		Consider(Circuit);
	}

	return true;
}

void FFPMPowerWarningProbe::Arm()
{
	/*
	 * NO HOOK. The armed line still prints, because the contract's one stated exception is exactly this:
	 * it is what separates "armed and saw nothing" from "never armed".
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] power-warning probe ARMED - READ ONLY, no hook. It samples every %.0f s for %.0f s "
		     "after each world load and answers one question: when the 'Fuse Blown' popup appears at "
		     "login, is any circuit ACTUALLY fuse-triggered? It deliberately does NOT hook "
		     "PowerCircuit_OnFuseSet - that is a BlueprintNativeEvent with an empty native body, so the "
		     "hook would never fire and its silence would read as 'no bug'. FPM.Power.Report samples on "
		     "demand."),
		GFPMPowerSampleIntervalSeconds, GFPMPowerWindowSeconds);
}

void FFPMPowerWarningProbe::OnWorldLoad(UWorld* World)
{
	StopSampler();

	GFPMPowerWorld = World;
	GFPMPowerElapsed = 0.0f;
	GFPMPowerHaveLast = false;
	GFPMPowerPeakTriggered = 0;

	if (World == nullptr) { return; }

	GFPMPowerTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float Delta) -> bool
		{
			UWorld* World = GFPMPowerWorld.Get();
			if (World == nullptr)
			{
				// The world went away mid-window. Not a finding, just the end of this sample run.
				return false;
			}

			GFPMPowerElapsed += Delta;
			const FFPMPowerSample S = Sample(World);
			GFPMPowerPeakTriggered = FMath::Max(GFPMPowerPeakTriggered, S.FuseTriggered);

			const bool bChanged = !GFPMPowerHaveLast || !SameAs(S, GFPMPowerLast);
			GFPMPowerLast = S;
			GFPMPowerHaveLast = true;

			/*
			 * ONLY ON CHANGE. The interesting shape is the TRANSITION — circuits appearing as they
			 * replicate, and a fuse count moving off or onto zero. Printing every sample would be 30
			 * near-identical lines per load, which is the log spam this mod exists to remove.
			 */
			if (bChanged && FPMDiag::IsOn(FPMDiag::EChannel::PowerWarning))
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] power probe t+%.0fs: %s"), GFPMPowerElapsed, *Describe(S));
			}

			if (GFPMPowerElapsed < GFPMPowerWindowSeconds)
			{
				return true;
			}

			/*
			 * ★ THE VERDICT LINE. Warning level and NOT gated by the channel when the answer is the
			 * interesting one, because this is the whole reason the probe exists and a reader should not
			 * have to have known to turn a channel on beforehand.
			 */
			if (S.bSubsystem && S.PowerCircuits > 0 && GFPMPowerPeakTriggered == 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] power probe VERDICT: across %.0f s and %d power circuit(s), NOT ONE fuse "
					     "was ever triggered. If the 'Fuse Blown' popup appeared during this window, the "
					     "warning is a FALSE POSITIVE and the origin is named. If it did not appear, this "
					     "window simply saw a healthy grid and settles nothing."),
					GFPMPowerWindowSeconds, S.PowerCircuits);
			}
			else if (GFPMPowerPeakTriggered > 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] power probe VERDICT: %d circuit(s) DID report a triggered fuse during the "
					     "window (currently %d of %d). The warning is telling the truth about at least one "
					     "circuit. DO NOT SUPPRESS IT - find the derelict over-capacity circuit instead."),
					GFPMPowerPeakTriggered, S.FuseTriggered, S.PowerCircuits);
			}
			else
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] power probe VERDICT: INCONCLUSIVE - %s. The probe ran; it could not see "
					     "the state. That is a finding about the instrument, not about the grid."),
					*Describe(S));
			}

			FPMOverlay::Post(TEXT("power-probe"), Describe(S));
			return false;
		}),
		GFPMPowerSampleIntervalSeconds);
}

void FFPMPowerWarningProbe::Disarm()
{
	StopSampler();
	GFPMPowerWorld.Reset();
}

void FFPMPowerWarningProbe::ReportNow()
{
	UWorld* World = GFPMPowerWorld.Get();
	if (World == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] power probe: no world. Load a save first - there are no circuits in the menu."));
		return;
	}

	const FFPMPowerSample S = Sample(World);

	/*
	 * ★ FOLD THE ON-DEMAND SAMPLE INTO THE PEAK, and say what the peak actually covers.
	 *
	 * The old line took FMath::Max(peak, this sample) for the printout and threw the result away, so a
	 * tripped fuse caught by an on-demand report vanished from the next one. And it called the number
	 * "this session" while the only thing feeding it was the %.0f s window after load — past that the
	 * value froze, and a frozen zero reads exactly like a measured zero.
	 *
	 * Storing it fixes both: every sample this probe ever takes now raises the peak, and the label names
	 * its real coverage instead of implying continuous watching. It is deliberately NOT a continuous
	 * poll — that would cost a circuit walk forever to answer a question that only matters when Ant is
	 * looking at the popup.
	 */
	GFPMPowerPeakTriggered = FMath::Max(GFPMPowerPeakTriggered, S.FuseTriggered);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] power probe (on demand): %s. Highest seen so far: %d - that covers the %.0f s after "
		     "load plus every FPM.Power.Report since, NOT the time in between."),
		*Describe(S), GFPMPowerPeakTriggered, GFPMPowerWindowSeconds);
	FPMOverlay::Post(TEXT("power-probe"), Describe(S));
}

void FFPMPowerWarningProbe::ReportTrippedToChat()
{
	UWorld* World = GFPMPowerWorld.Get();
	if (World == nullptr)
	{
		FPMChat(TEXT("[FPM] power check: no world loaded yet. Load a save first - there are no circuits "
		             "in the menu."));
		return;
	}

	int32 Examined = 0;
	int32 Unreadable = 0;
	TArray<int32> TrippedIDs;
	const bool bOk = EnumerateTrippedCircuits(World, Examined, Unreadable, TrippedIDs);

	if (!bOk)
	{
		// Distinct from "0 tripped" — the subsystem was not even there to ask.
		FPMChat(TEXT("[FPM] power check: no circuit subsystem yet. CANNOT ANSWER - try again in a "
		             "moment, this is not the same as a clean grid."));
		return;
	}

	/*
	 * ★ THE LOG GETS THE FULL DETAIL, CHAT GETS THE VERDICT — the same split FPMResidueSentinel already
	 * uses (FPMResidueSentinel.cpp), and for the same reason Ant gave there: "log and chat then". A dev
	 * reading FactoryGame.log gets every ID either way; a player gets the short answer only.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] power check (chat verb): %d power circuit(s) examined, %d tripped, %d unreadable. "
		     "Tripped IDs: %s"),
		Examined, TrippedIDs.Num(), Unreadable,
		TrippedIDs.Num() > 0
			? *FString::JoinBy(TrippedIDs, TEXT(", "), [](int32 ID) { return FString::FromInt(ID); })
			: TEXT("(none)"));

	// ★ SAYS SOMETHING EVEN WHEN NOTHING IS TRIPPED — silence here would read as a broken command.
	if (TrippedIDs.Num() == 0)
	{
		FPMChatf(TEXT("[FPM] power check: no tripped fuses. (%d circuit(s) examined, 0 tripped, %d "
		              "unreadable)"),
			Examined, Unreadable);
		return;
	}

	FPMChatf(TEXT("[FPM] power check: %d circuit(s) have a TRIPPED FUSE - ID(s): %s"),
		TrippedIDs.Num(),
		*FString::JoinBy(TrippedIDs, TEXT(", "), [](int32 ID) { return FString::FromInt(ID); }));

	// ★ COVERAGE, ALWAYS — so "0 tripped" can be told apart from a probe that saw nothing at all.
	FPMChatf(TEXT("[FPM] power check coverage: %d examined, %d tripped, %d unreadable."),
		Examined, TrippedIDs.Num(), Unreadable);
}

/*
 * `FPM.Power.Report` — for the moment the popup is actually on screen.
 *
 * The sampling window covers login, which is when Ant sees it. This exists for the other case: she is
 * mid-session, the popup appears, and the answer is wanted for THAT instant rather than for the minute
 * after the last load.
 */
static FAutoConsoleCommandWithOutputDevice GFPMPowerReportCmd(
	TEXT("FPM.Power.Report"),
	TEXT("Sample the power circuits now and report how many actually have a triggered fuse."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMPowerWarningProbe::ReportNow();
	}));

/*
 * `FPM.Power.Chat` — the CHAT-FACING verb (board m5663571 §7.2). Same read-only sample as
 * `FPM.Power.Report`, routed to the surface Ant actually reads: only the tripped circuits, plus a
 * coverage line, posted to the local in-game chat window.
 */
static FAutoConsoleCommandWithOutputDevice GFPMPowerChatCmd(
	TEXT("FPM.Power.Chat"),
	TEXT("Enumerate power circuits now and post ONLY the tripped ones (plus coverage) to your in-game "
	     "chat. Client only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);

		/*
		 * FPMChat() itself no-ops on a dedicated server (FPMChatRelay.cpp) because there is no local chat
		 * window to write into. Saying so explicitly here beats going silent — the same reasoning
		 * FPM.Chat.Test uses for the same case.
		 */
		if (IsRunningDedicatedServer())
		{
			Ar.Log(TEXT("[FPM] power check: chat is client-only and this is a dedicated server. Use "
			            "FPM.Power.Report here instead, or run FPM.Power.Chat from a connected client."));
			return;
		}
		FFPMPowerWarningProbe::ReportTrippedToChat();
	}));
