// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMRailConnectionGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGRailroadTrackConnectionComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
// For the hologram-vs-placed split: a preview's unwired connection is expected, a placed buildable's
// is not, and only one of those deserves an unthrottled Error.
#include "Hologram/FGHologram.h"

#include <atomic>

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * Averted asserts, and calls forwarded untouched. Atomic because rail connections register during
	 * world construction and blueprint placement, neither of which is guaranteed to be one thread.
	 */
	std::atomic<int32> GFPMRailAverted{0};
	std::atomic<int32> GFPMRailPassed{0};

	/**
	 * Distinct owner classes seen on the NON-HOLOGRAM branch. Bounded, and game-thread only — a TSet is
	 * not atomic, and this hook is explicitly documented as reachable from more than one thread.
	 *
	 * It exists so the throttle added on 2026-08-11 cannot hide the thing the unthrottled version was
	 * protecting: a SECOND, different class turning up. Repeats of a known class are noise; a new class
	 * is the finding.
	 */
	TSet<FName> GFPMRailNonHologramClasses;
}

FFPMRailConnectionGuard& FFPMRailConnectionGuard::Get()
{
	static FFPMRailConnectionGuard Instance;
	return Instance;
}

void FFPMRailConnectionGuard::GetCounts(int32& OutAverted, int32& OutPassedThrough)
{
	OutAverted = GFPMRailAverted.load();
	OutPassedThrough = GFPMRailPassed.load();
}

void FFPMRailConnectionGuard::Arm()
{
	/*
	 * Plain SUBSCRIBE_METHOD, not the _VIRTUAL form: `GetOpposite()` is a non-virtual const member
	 * (FGRailroadTrackConnectionComponent.h:151) and nothing in FactoryGame overrides it.
	 *
	 * ⚠ HOT-ISH PATH. This fired ~2,100 times inside 26 ms at server start in the field, so the
	 * pass-through branch must stay two pointer compares and an atomic increment. No strings, no
	 * GetName(), no console read, unless the guard actually fires.
	 */
	auto OnGetOpposite = [](auto& Scope, const UFGRailroadTrackConnectionComponent* Self)
	{
		if (!Self) { return; }

		const AFGBuildableRailroadTrack* Track = Self->GetTrack();

		/*
		 * ★ VANILLA'S OWN PRECONDITION, REPRODUCED — this is what makes the guard narrow rather than a
		 * guess. It asserts `GetTrack()->GetConnection(1) == this`, which requires BOTH that a track
		 * exists AND that this component is one of that track's two connections. Anything else is the
		 * state that aborts.
		 */
		const bool bWired = Track && (Track->GetConnection(0) == Self || Track->GetConnection(1) == Self);
		if (bWired)
		{
			++GFPMRailPassed;   // real connection: vanilla behaviour, bit-for-bit
			return;
		}

		/*
		 * ⚠ THE OVERRIDE COMES FIRST AND IS NEVER CONDITIONAL ON DIAGNOSTICS. Everything below it is
		 * reporting. If a future edit ever puts the Override inside a logging branch, turning the channel
		 * off would turn the crash back on — the exact shape the RPC gate's comment warns about.
		 */
		Scope.Override(nullptr);

		const int32 N = ++GFPMRailAverted;

		/*
		 * ★ REFINEMENT OVER THE OLD MOD'S VERSION — SPLIT THE EXPECTED CASE FROM THE ALARMING ONE.
		 *
		 * The old guard treated every unwired connection identically and throttled them all together.
		 * But there are two very different populations in here:
		 *
		 *   - a HOLOGRAM's duplicated connection. Expected, benign, and it arrives ~2,000 times in a
		 *     burst at server start. Sampling it is right; logging every one is the freeze the schematic
		 *     probe already taught us about.
		 *   - a connection on a REAL PLACED BUILDABLE that is somehow not wired to its track. That should
		 *     not be possible, and returning nullptr for it means we are silently papering over a genuine
		 *     bug in placed track. Throttling that to 1-in-50 could hide the only instance in a session.
		 *
		 * Same guard, same override — the SAFE action is identical and stays unconditional. What differs
		 * is how loudly we say it, and that distinction is only makeable here, at the call site, where
		 * the owner is in hand.
		 */
		const AActor* Owner = Self->GetOwner();
		const bool bHologram = Owner && Owner->IsA<AFGHologram>();

		if (!bHologram)
		{
			/*
			 * ★ "BELIEVED-UNREACHABLE" WAS WRONG, AND THE MEASUREMENT IS WHAT SAYS SO.
			 *
			 * This branch was UNTHROTTLED on the stated reasoning that a non-hologram owner should be
			 * impossible, and that throttling "could hide the only instance in a session". Ant's
			 * dedicated-server log, 2026-08-11, 0.11.13:
			 *
			 *     2034  LogFicsitsPerformanceManager: Error: [FPM] rail guard
			 *
			 * 2,034 Error lines — the largest single FPM contributor in the whole server log, and all of
			 * them this branch, all naming the same owner class (Build_RailroadTrack_C), most inside the
			 * same millisecond during load. There was never "the only instance" to hide.
			 *
			 * The premise was an assumption presented as a boundary condition, and an unthrottled Error
			 * resting on it made the log unreadable — the exact harm the guard's sibling fixes exist to
			 * undo, committed by a guard. It is also the failure the arm line ALREADY predicted in the
			 * next breath, quoting 1,900-2,550 averted asserts per server start as EXPECTED VOLUME.
			 * Expected volume must never be one Error per occurrence.
			 *
			 * THE GOAL SURVIVES, THE FLOOD DOES NOT. The first few are still unthrottled and still Error,
			 * so a genuinely rare instance cannot be lost. Then it throttles, and — because the original
			 * worry was about missing a DISTINCT case rather than a repeat — every new owner class is
			 * reported on sight regardless of count. A second class appearing is the thing worth knowing;
			 * the two-thousandth repeat of the first is not.
			 */
			constexpr int32 UnthrottledHead = 5;
			const FName OwnerClass = Owner ? Owner->GetClass()->GetFName() : FName(TEXT("<none>"));

			bool bNewClass = false;
			if (IsInGameThread() && GFPMRailNonHologramClasses.Num() < 16)
			{
				bNewClass = !GFPMRailNonHologramClasses.Contains(OwnerClass);
				if (bNewClass) { GFPMRailNonHologramClasses.Add(OwnerClass); }
			}

			const bool bSay = N <= UnthrottledHead || bNewClass || (N % FPMLog::ThrottleRoutine) == 0;
			if (bSay && FPMDiag::IsOn(FPMDiag::EChannel::RailGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] rail guard: an unwired connection on a NON-HOLOGRAM owner (%s) - averted "
					     "assert #%d%s. This is NOT the expected blueprint-preview case: a placed "
					     "buildable's rail connection is not wired to its own track. The guard returned "
					     "null and the server survived, but something is wrong with real track. Repeats "
					     "are throttled - every DISTINCT owner class is still reported on sight. "
					     "FPM.RailGuard.Report has the totals; FPM.RailGuard.Scan lists exact world "
					     "positions."),
					*OwnerClass.ToString(), N,
					bNewClass && N > UnthrottledHead ? TEXT(" [FIRST OF THIS CLASS]") : TEXT(""));
			}
			return;
		}

		// Cheap test first: the modulo is a register op, IsOn reads a console variable.
		if ((N == 1 || (N % FPMLog::ThrottleNotable) == 0)
			&& FPMDiag::IsOn(FPMDiag::EChannel::RailGuard))
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] rail guard: GetOpposite() on an unwired HOLOGRAM connection (#%d averted, %d "
				     "passed through, owner %s) returned null instead of asserting. A mod - "
				     "DynamicTrainRoutes in the observed case - walks blueprint-hologram rail connections "
				     "as if they were placed track; vanilla asserts and kills the server."),
				N, GFPMRailPassed.load(), *Owner->GetClass()->GetName());
		}
	};

	GetOppositeHandle = FPM_SUBSCRIBE("rail-connection-guard", UFGRailroadTrackConnectionComponent::GetOpposite, OnGetOpposite);

	if (GetOppositeHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] rail guard ARMED - GetOpposite() on an unwired/hologram rail connection returns null "
			     "instead of asserting. Measured in the field on the OLD mod: 1,900-2,550 averted asserts per "
			     "server start, 23,450 across 11 sessions. A properly-wired connection is forwarded untouched, "
			     "so live rail pathfinding is unchanged."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] rail guard NOT armed - hook install FAILED on "
			     "UFGRailroadTrackConnectionComponent::GetOpposite. The unwired/hologram-connection assert "
			     "this guard exists to avert is UNPROTECTED this session."));
	}
}

void FFPMRailConnectionGuard::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct here even though Arm() subscribed with plain SUBSCRIBE_METHOD, not
	 * the _VIRTUAL form (see the note at Arm(), above): both drive the same HookInvoker<decltype(&M),
	 * &M>, and RemoveHandler clears the BEFORE and AFTER maps alike, uninstalling the detour once both
	 * are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (GetOppositeHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UFGRailroadTrackConnectionComponent::GetOpposite, GetOppositeHandle);
		GetOppositeHandle.Reset();
	}
}

void FFPMRailConnectionGuard::LogReport(FOutputDevice* Ar)
{
	int32 Averted = 0, PassedThrough = 0;
	GetCounts(Averted, PassedThrough);

	const FString Line = FString::Printf(
		TEXT("[FPM] rail guard: %d assert(s) averted · %d call(s) passed through untouched. Both numbers, "
		     "never just the first - the pass-through count is what makes the averted count readable."),
		Averted, PassedThrough);

	if (Ar != nullptr)
	{
		Ar->Log(Line);
	}
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
}

void FFPMRailConnectionGuard::RunScan(FOutputDevice* Ar)
{
	// Every UE_LOG below also reaches Ar for the rest of this call - see FPMConsoleEcho.h. Without this,
	// Display-level logs never reach the in-game console and the command looks dead, the same trap the
	// old FPM.Rail.Report promise walked into.
	FPMScopedConsoleEcho Echo(Ar);

	UWorld* World = GEngine != nullptr ? GEngine->GetCurrentPlayWorld() : nullptr;
	if (World == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] rail scan: no world right now (are you in the main menu?)."));
		return;
	}

	/*
	 * ★ WHICH MACHINE IS ANSWERING, STATED UP FRONT. The guard arms with `Side()==Any` - client AND
	 * dedicated server both run it - so a scan run on a client is not guaranteed to see what the
	 * server's own authoritative world sees. Printing this is what stops a client-side "looks fine"
	 * from being mistaken for the server's picture, or vice versa.
	 */
	const ENetMode NetMode = World->GetNetMode();
	const TCHAR* SideText =
		NetMode == NM_DedicatedServer ? TEXT("DEDICATED SERVER (authoritative - this is what gets saved)") :
		NetMode == NM_ListenServer    ? TEXT("LISTEN SERVER (authoritative, also hosts a local player)") :
		NetMode == NM_Client          ? TEXT("CLIENT (NOT authoritative - the dedicated server's own scan "
		                                      "can see a different picture if the damage lives there)") :
		                                 TEXT("STANDALONE (single save, this machine IS authoritative)");

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] rail scan: running on %s. READ ONLY - this reports positions, it repairs nothing."),
		SideText);

	/*
	 * The five owner classes the 2026-08-15 triage's log grep found damaged (rail-save-damage.md §2,
	 * LOG-tier, byte-verified). Used below to report which of them THIS sweep actually reaches - a
	 * measurement, not the design pass's guess about it.
	 */
	static const FName GKnownDamagedClasses[] = {
		FName(TEXT("Build_RailroadTrack_C")),
		FName(TEXT("Build_RailroadTrackIntegrated_C")),
		FName(TEXT("Build_RailroadEndStop_C")),
		FName(TEXT("Build_Road04_C")),
		FName(TEXT("Build_Road01_C")),
	};
	// The two classes with a stated, unresolved doubt (rail-repair-design.md §2c): a dedicated
	// UFGRoadConnectionComponent class exists and nothing read in that pass explains why a Road
	// buildable would own a railroad connection component instead.
	static const FName GLowerConfidenceClasses[] = { FName(TEXT("Build_Road04_C")), FName(TEXT("Build_Road01_C")) };

	TSet<FName> ClassesSeenBySweep;
	int32 TracksExamined = 0;
	int32 SlotsExamined = 0;
	int32 SlotsDamaged = 0;
	int32 SlotsUnclassifiable = 0;

	for (TActorIterator<AFGBuildableRailroadTrack> It(World); It; ++It)
	{
		AFGBuildableRailroadTrack* Track = *It;
		if (!IsValid(Track)) { continue; }
		++TracksExamined;

		const FName OwnerClass = Track->GetClass()->GetFName();
		ClassesSeenBySweep.Add(OwnerClass);

		bool bLowerConfidence = false;
		for (const FName& LowerConf : GLowerConfidenceClasses)
		{
			if (LowerConf == OwnerClass) { bLowerConfidence = true; break; }
		}

		for (int32 Slot = 0; Slot < 2; ++Slot)
		{
			++SlotsExamined;
			UFGRailroadTrackConnectionComponent* C = Track->GetConnection(Slot);
			if (C == nullptr)
			{
				// §2a of the design pass: every AFGBuildableRailroadTrack constructs exactly 2 connection
				// subobjects, unconditionally, in its own constructor, and nothing else in the tree
				// reassigns mConnections. A null slot here contradicts that - reported, not skipped.
				++SlotsUnclassifiable;
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] rail scan: UNCLASSIFIABLE - %s '%s' slot %d has a NULL connection, which "
					     "should be structurally impossible."),
					*OwnerClass.ToString(), *Track->GetName(), Slot);
				continue;
			}

			// The exact bWired test Arm()'s hook uses, run proactively instead of reactively.
			const AFGBuildableRailroadTrack* CTrack = C->GetTrack();
			const bool bWired = CTrack && (CTrack->GetConnection(0) == C || CTrack->GetConnection(1) == C);
			if (bWired) { continue; }

			++SlotsDamaged;

			/*
			 * Which of the two wiring halves failed (task's own framing): `T->mConnections[Slot]==C`
			 * holds HERE by construction - C came directly from Track's own array two lines above, and
			 * §2a found nothing else in the tree ever reassigns it. So for a connection reached this
			 * way, the track side cannot be the broken one; only `C->mTrackPosition.Track==T` can be,
			 * and that is exactly the `CTrack` compare above.
			 */
			const FVector Pos = C->GetConnectorLocation();
			const TCHAR* Confidence = bLowerConfidence
				? TEXT(" [LOWER CONFIDENCE - Road-named owner class; unconfirmed whether this class "
				       "belongs in the railroad connection model at all, see rail-repair-design.md §2c]")
				: TEXT("");

			if (CTrack == nullptr)
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] rail scan: DAMAGED %s '%s' slot %d at X=%.1f Y=%.1f Z=%.1f - connection "
					     "side broken: mTrackPosition.Track is NULL. Track side intact: this track's own "
					     "mConnections[%d] correctly lists the connection.%s"),
					*OwnerClass.ToString(), *Track->GetName(), Slot, Pos.X, Pos.Y, Pos.Z, Slot, Confidence);
			}
			else
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] rail scan: DAMAGED %s '%s' slot %d at X=%.1f Y=%.1f Z=%.1f - connection "
					     "side broken: mTrackPosition.Track points at a DIFFERENT track ('%s'), not this "
					     "one. Track side intact: this track's own mConnections[%d] correctly lists the "
					     "connection.%s"),
					*OwnerClass.ToString(), *Track->GetName(), Slot, Pos.X, Pos.Y, Pos.Z,
					*CTrack->GetName(), Slot, Confidence);
			}
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] rail scan coverage: %d AFGBuildableRailroadTrack actor(s) examined (%d connection "
		     "slot(s)) - %d damaged, %d unclassifiable."),
		TracksExamined, SlotsExamined, SlotsDamaged, SlotsUnclassifiable);

	// ★ THE BLIND SPOT, MEASURED THIS RUN RATHER THAN ASSERTED. A scan that stays silent about what it
	// cannot see reads as a clean bill of health, which is exactly the false comfort this line exists to
	// prevent.
	TArray<FString> ClassesNotReached;
	for (const FName& Known : GKnownDamagedClasses)
	{
		if (!ClassesSeenBySweep.Contains(Known)) { ClassesNotReached.Add(Known.ToString()); }
	}

	if (ClassesNotReached.Num() > 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] rail scan coverage: BLIND SPOT this run - %d of %d owner class(es) the "
			     "2026-08-15 triage log found damaged were NEVER encountered by this "
			     "AFGBuildableRailroadTrack sweep: %s. Either none are currently loaded, or they do not "
			     "derive from AFGBuildableRailroadTrack at all - this scan cannot see them either way, "
			     "and their damage counts from FPM.RailGuard.Report are NOT reflected in the coverage "
			     "line above."),
			ClassesNotReached.Num(), UE_ARRAY_COUNT(GKnownDamagedClasses),
			*FString::Join(ClassesNotReached, TEXT(", ")));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] rail scan coverage: all %d owner class(es) named in the 2026-08-15 triage log "
			     "were reached by this sweep this run - no blind spot found."),
			UE_ARRAY_COUNT(GKnownDamagedClasses));
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] rail scan: this is DISTINCT damaged connections, not the same figure as "
		     "FPM.RailGuard.Report's 'averted' count - that counter increments once per GetOpposite() "
		     "CALL this session, and the same connection can be called more than once, so the two "
		     "numbers are not expected to match."));
}

/*
 * `FPM.RailGuard.Report` — takes the output device so it prints in the console she is looking at as
 * well as the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command
 * that answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMRailGuardReportCmd(
	TEXT("FPM.RailGuard.Report"),
	TEXT("Print how many unwired/hologram rail connection asserts this guard has averted, and how many "
	     "calls it passed through untouched, this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMRailConnectionGuard::LogReport(&Ar);
	}));

/*
 * `FPM.RailGuard.Scan` — walks the currently-loaded world and prints a WORLD POSITION for every damaged
 * rail connection, plus which side of the wiring broke and an honest coverage line. READ ONLY: it prints,
 * it never writes. See FFPMRailConnectionGuard::RunScan for the coverage/blind-spot detail.
 */
static FAutoConsoleCommandWithOutputDevice GFPMRailGuardScanCmd(
	TEXT("FPM.RailGuard.Scan"),
	TEXT("Scan every loaded rail track for damaged connections and print a world position for each one, "
	     "read-only."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMRailConnectionGuard::RunScan(&Ar);
	}));
