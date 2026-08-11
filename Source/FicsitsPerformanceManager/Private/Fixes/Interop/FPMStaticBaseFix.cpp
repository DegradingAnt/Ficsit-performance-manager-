// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMStaticBaseFix.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"

#include "FGColoredInstanceMeshProxy.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"

#include <atomic>

namespace
{
	/*
	 * ★ THIS FIX WAS COMPLETELY UNOBSERVABLE UNTIL 2026-08-11, and that is why finding it needed a
	 * player's report rather than a log. It had no counter, no log line and no report command — grepping
	 * the whole file for `UE_LOG`, `Diag` or a counter returned NOTHING.
	 *
	 * So there was no way, from a log, to tell "rewrote 4,000 adjustments" from "never fired once" — and
	 * the second is what a joined client actually gets, because the handler returns on
	 * `GetLocalRole() != ROLE_Authority`. A fix that cannot be observed cannot be reviewed, cannot be
	 * bisected, and cannot be defended when it is blamed. It is the dead-instrument shape with the
	 * instrument missing altogether.
	 */
	std::atomic<int64> GStaticBaseRewritten{0};   // adjustments actually converted to world space
	std::atomic<int64> GStaticBaseDeclined{0};    // reached the proxy test, then a guard refused

	/** Squared cm/s. Anything above this is a base that is genuinely in motion, not float noise. */
	constexpr double GMovingBaseVelocitySq = 1.0;

	/**
	 * The modes this fix is allowed to touch: a character actually STANDING on (or falling near) a
	 * factory piece.
	 *
	 * REFINED ON CARRY. The old form was
	 *     if (Mode == MOVE_Custom || (Mode != MOVE_Walking && Mode != MOVE_NavWalking && Mode != MOVE_Falling))
	 * whose first clause is provably dead — MOVE_Custom already fails all three inequalities. Stating
	 * the allowlist positively is identical in behaviour and does not invite a reader to wonder which
	 * of the two conditions is doing the work.
	 */
	bool IsGroundMode(const EMovementMode Mode)
	{
		return Mode == MOVE_Walking || Mode == MOVE_NavWalking || Mode == MOVE_Falling;
	}
}

FFPMStaticBaseFix& FFPMStaticBaseFix::Get()
{
	static FFPMStaticBaseFix Instance;
	return Instance;
}

void FFPMStaticBaseFix::Arm()
{
	UCharacterMovementComponent* Sample = GetMutableDefault<UCharacterMovementComponent>();

	auto OnSendAdjustment = [](auto& Scope, UCharacterMovementComponent* Move)
	{
		if (!Move || !Move->HasValidData()) { return; }

		/*
		 * EXPLICIT AUTHORITY GUARD, ADDED ON CARRY.
		 *
		 * The old version relied on GetPredictionData_Server_Character() returning null off the server.
		 * That is true today only because the engine calls SendClientAdjustment from the authority
		 * branch of TickComponent — an implicit guarantee, not a stated one. It is also fragile in a
		 * Debug build: GetPredictionData_Server() carries a checkSlow on ROLE_Authority, which is
		 * compiled out of Development and Shipping but live in Debug, so the "safe" null return would
		 * have been an assert instead.
		 *
		 * The project's own hard rule is to self-guard the side explicitly, every time. Doing it here
		 * costs one comparison on a path that already runs per-correction.
		 */
		const ACharacter* Owner = Move->GetCharacterOwner();
		if (!Owner || Owner->GetLocalRole() != ROLE_Authority) { return; }

		FNetworkPredictionData_Server_Character* Data = Move->GetPredictionData_Server_Character();
		if (!Data) { return; }

		FClientAdjustment& Adjustment = Data->PendingAdjustment;
		if (Adjustment.bAckGoodMove
			|| !Adjustment.bBaseRelativePosition
			|| !Adjustment.NewBase
			|| !Adjustment.NewBase->IsA<UFGColoredInstanceMeshProxy>())
		{
			return;
		}

		if (!IsGroundMode(Move->MovementMode)) { return; }

		/*
		 * MOVING BASES KEEP RELATIVE BASING — AND THIS IS THE BUG THE CARRY FIXED.
		 *
		 * The old form nested BOTH velocity tests inside `if (const AActor* BaseOwner = GetOwner())`,
		 * so a base component with no owning actor skipped the COMPONENT velocity test as well — even
		 * though that test never needed an owner. The guard's stated intent is "any measurable base
		 * velocity", and it was silently conditioned on something unrelated.
		 *
		 * Narrow in practice, because a scene component almost always has an owner. It is fixed anyway:
		 * a guard that does not do what its comment says is the class of defect this rewrite exists to
		 * remove, and the failure mode here is pinning a rider to where a moving platform WAS.
		 */
		if (Adjustment.NewBase->GetComponentVelocity().SizeSquared() > GMovingBaseVelocitySq)
		{
			GStaticBaseDeclined.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (const AActor* BaseOwner = Adjustment.NewBase->GetOwner())
		{
			if (BaseOwner->GetVelocity().SizeSquared() > GMovingBaseVelocitySq)
			{
				GStaticBaseDeclined.fetch_add(1, std::memory_order_relaxed);
				return;
			}
		}

		// Mutate then fall through: vanilla sends the adjustment we just rewrote.
		FVector WorldLocation;
		if (MovementBaseUtility::TransformLocationToWorld(
				Adjustment.NewBase, Adjustment.NewBaseBoneName, Adjustment.NewLoc, WorldLocation))
		{
			Adjustment.NewLoc = WorldLocation;
			Adjustment.bBaseRelativePosition = false;
			GStaticBaseRewritten.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			// The transform refused. Counted, because a silent failure here looks exactly like a fix
			// that simply never had work to do, and those need different responses.
			GStaticBaseDeclined.fetch_add(1, std::memory_order_relaxed);
		}
	};

	AdjustmentHookHandle = FPM_SUBSCRIBE_VIRTUAL("static-base",
		UCharacterMovementComponent::SendClientAdjustment, Sample, OnSendAdjustment);
}

void FFPMStaticBaseFix::Disarm()
{
	/*
	 * ★ `UNSUBSCRIBE_METHOD` IS CORRECT HERE EVEN THOUGH THE SUBSCRIBE WAS _VIRTUAL, and that is worth
	 * stating because SML ships no `UNSUBSCRIBE_METHOD_VIRTUAL` and its absence reads like a gap.
	 *
	 * Both macros drive the same template. `SUBSCRIBE_METHOD_VIRTUAL` differs only in handing
	 * `InstallHook` a sample object so the vtable slot can be resolved; the handler itself goes into
	 * `HookInvoker<decltype(&M), &M>`'s array either way, and `UNSUBSCRIBE_METHOD` removes from that
	 * same array (`NativeHookManager.h:649-650`).
	 *
	 * ⚠ GUARDED ON IsValid() because the editor path installs nothing and hands back an invalid handle.
	 * RemoveHandler would then walk arrays SML never allocated — the trap FPMBlueprintSweepGate
	 * documents at its own unsubscribe site.
	 */
	if (AdjustmentHookHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UCharacterMovementComponent::SendClientAdjustment, AdjustmentHookHandle);
		AdjustmentHookHandle.Reset();
	}
}

void FFPMStaticBaseFix::OnWorldLoad(UWorld* World)
{
	if (World == nullptr) { return; }

	/*
	 * ★ A JOINED CLIENT CANNOT USE THIS FIX, SO IT SHOULD NOT CARRY THE DETOUR.
	 *
	 * `SendClientAdjustment` is the server's correction path. The handler's first real test is
	 * `Owner->GetLocalRole() != ROLE_Authority`, and on a machine that merely JOINED a server nothing
	 * the local player controls is ever `ROLE_Authority` — so the handler returns every single time,
	 * having been reached through a funchook trampoline on a function the engine calls per correction.
	 *
	 * Ant, 2026-08-11, on a dedicated-server client: `FPM.Fix.StaticBase 0` restored her movement. The
	 * gate cancels nothing on that machine, so what she removed was the detour, not the behaviour.
	 *
	 * `NM_Client` is exactly the case with no authority. Standalone, listen-host and dedicated all keep
	 * the fix, which is where the bug it repairs actually lives.
	 *
	 * ⚠ THIS CANNOT BE DONE IN Arm(). There is no world at module startup, so the net mode is unknowable
	 * there — the same reason `FFPMWwiseServerGate` can use `IsRunningDedicatedServer()` at registration
	 * time and this fix cannot. World load is the first moment the answer exists, and it is re-evaluated
	 * on every load because one process can travel between a listen host and a joined client.
	 */
	const ENetMode NetMode = World->GetNetMode();

	if (NetMode == NM_Client)
	{
		if (AdjustmentHookHandle.IsValid())
		{
			Disarm();
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] static-base: this world is NM_Client, where nothing local is ever "
				     "ROLE_Authority and SendClientAdjustment's handler can only ever return. The hook "
				     "is REMOVED rather than left as a detour on the engine's per-correction path. It "
				     "comes back automatically on a standalone, listen-host or dedicated world."));
		}
		return;
	}

	if (!AdjustmentHookHandle.IsValid())
	{
		Arm();
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] static-base: re-armed for a world with local authority (net mode %d)."),
			static_cast<int32>(NetMode));
	}
}

void FFPMStaticBaseFix::ReportNow()
{
	const int64 Rewritten = GStaticBaseRewritten.load(std::memory_order_relaxed);
	const int64 Declined  = GStaticBaseDeclined.load(std::memory_order_relaxed);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] static-base: %lld adjustment(s) rewritten to world space, %lld declined by a guard "
		     "after reaching the instancing-proxy test."),
		static_cast<long long>(Rewritten), static_cast<long long>(Declined));

	/*
	 * BOTH ZERO IS A REAL ANSWER AND MUST NOT READ AS HEALTH. It means the hook is installed and has
	 * never once reached its own subject — which on a joined client is the CORRECT outcome and on a
	 * server is a finding.
	 */
	UE_CLOG(Rewritten == 0 && Declined == 0, LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   both zero: the hook has never reached a UFGColoredInstanceMeshProxy base. On a "
		     "joined client that is expected and the hook should have been removed at world load. "
		     "Anywhere else it means this fix has had nothing to do all session."));
}

static FAutoConsoleCommand GFPMStaticBaseReportCmd(
	TEXT("FPM.StaticBase.Report"),
	TEXT("Static-base fix: adjustments rewritten vs declined. Both zero means it never reached its subject."),
	FConsoleCommandDelegate::CreateStatic(&FFPMStaticBaseFix::ReportNow));
