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
			// Believed-unreachable ⇒ UNTHROTTLED, per the FPMLog policy's stated third tier.
			if (FPMDiag::IsOn(FPMDiag::EChannel::RailGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Error,
					TEXT("[FPM] rail guard: an unwired connection on a NON-HOLOGRAM owner (%s) - averted "
					     "assert #%d. This is NOT the expected blueprint-preview case: a placed buildable's "
					     "rail connection is not wired to its own track. The guard returned null and the "
					     "server survived, but something is wrong with real track and this line is the only "
					     "evidence of it."),
					Owner ? *Owner->GetClass()->GetName() : TEXT("<none>"), N);
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
