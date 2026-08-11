// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMRailConnectionGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGRailroadTrackConnectionComponent.h"
#include "Buildables/FGBuildableRailroadTrack.h"
// For the hologram-vs-placed split: a preview's unwired connection is expected, a placed buildable's
// is not, and only one of those deserves an unthrottled Error.
#include "Hologram/FGHologram.h"

#include <atomic>

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
					     "are throttled - every DISTINCT owner class is still reported on sight, and "
					     "FPM.Rail.Report has the totals."),
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

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] rail guard ARMED - GetOpposite() on an unwired/hologram rail connection returns null "
		     "instead of asserting. Measured in the field on the OLD mod: 1,900-2,550 averted asserts per "
		     "server start, 23,450 across 11 sessions. A properly-wired connection is forwarded untouched, "
		     "so live rail pathfinding is unchanged."));
}

void FFPMRailConnectionGuard::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe: both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps
	 * alike, uninstalling the detour once both are empty (NativeHookManager.h:359-378).
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
