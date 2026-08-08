// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMStaticBaseFix.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"

#include "FGColoredInstanceMeshProxy.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
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
			return;
		}
		if (const AActor* BaseOwner = Adjustment.NewBase->GetOwner())
		{
			if (BaseOwner->GetVelocity().SizeSquared() > GMovingBaseVelocitySq) { return; }
		}

		// Mutate then fall through: vanilla sends the adjustment we just rewrote.
		FVector WorldLocation;
		if (MovementBaseUtility::TransformLocationToWorld(
				Adjustment.NewBase, Adjustment.NewBaseBoneName, Adjustment.NewLoc, WorldLocation))
		{
			Adjustment.NewLoc = WorldLocation;
			Adjustment.bBaseRelativePosition = false;
		}
	};

	FPM_SUBSCRIBE_VIRTUAL("static-base", UCharacterMovementComponent::SendClientAdjustment, Sample, OnSendAdjustment);
}
