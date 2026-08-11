// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * STATIC-BASE MOVEMENT FIX — stops rubber-banding while standing on factory pieces.
 *
 * THE BUG (engine source, CharacterMovementComponent.cpp:11007): when a movement correction is
 * base-RELATIVE and the client cannot resolve the base's net reference, it discards the ENTIRE
 * correction. UFGColoredInstanceMeshProxy — the instanced-rendering proxy on foundations and walls —
 * is exactly that case: Movable mobility for rendering reasons, not net-addressable, yet immobile in
 * gameplay. Roughly 550 corrections were dropped per session. World-space corrections are applied by
 * clients even when the base cannot be resolved.
 *
 * THE FIX: rewrite the server's PendingAdjustment so a proxy-based relative correction becomes
 * world-space, using the server's own resolved base transform. NewBase stays set, so a client that CAN
 * resolve the proxy keeps normal basing.
 *
 * ⚠ HOOK TARGET IS DELIBERATE AND WAS PAID FOR. v0.3.0 hooked MovementBaseUtility::IsDynamicBase — a
 * function so small that funchook's jump patch corrupted it into trampoline recursion, crashing on ANY
 * world load including the menu scene. This hooks UCharacterMovementComponent::SendClientAdjustment
 * instead: a large virtual, and the nearest caller that reproduces the same outcome safely.
 *
 * VERIFIED 2026-08-08: nothing in FactoryGame overrides SendClientAdjustment, so the base-class vtable
 * entry is the one that runs. SUBSCRIBE_METHOD_VIRTUAL only patches the class it is given — an
 * override that did not call Super would silently bypass this hook, so that absence is load-bearing
 * and must be re-checked if CSS ever ships an FGCharacterMovementComponent override.
 *
 * TWO BOUNDARIES, BOTH EARNED BY REGRESSIONS:
 *  - GROUND MODES ONLY. Ant, 2026-07-21: "sometimes the player glitches through hyper tubes when using
 *    modded ones". A character in a custom movement mode is not standing on anything, and rewriting a
 *    correction to world-space mid-tube fights the tube's own path-following at MK2 speeds.
 *  - MOVING BASES KEEP RELATIVE BASING. Ant: "elevators are really laggy and weird". The original
 *    boundary assumed elevators never base on a colored-instance proxy — wrong, because elevator
 *    cabins are COLORABLE, so their meshes render through exactly this proxy class. This fix's premise
 *    is "immobile in gameplay", so the premise is now enforced literally rather than assumed.
 */
class FFPMStaticBaseFix final : public IFPMFix
{
public:
	static FFPMStaticBaseFix& Get();

	virtual const TCHAR* Name() const override { return TEXT("static-base"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** OriginNamed: clients cannot net-resolve an immobile instancing-proxy base, so the correction targets a base the
	 * client can never bind -- the cause is named, not merely the symptom. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::StaticBase; }
	virtual void Arm() override;

	/**
	 * Removes the movement hook.
	 *
	 * ⚠ WITHOUT IT, `FPMFixes::DisarmAll()` REPORTS THIS FIX DISARMED WHILE IT KEEPS REWRITING EVERY
	 * CLIENT ADJUSTMENT. Harmless at process exit — the only place DisarmAll has ever been called from,
	 * and why the omission survived — but it is what blocks P4.2's master OFF switch from meaning
	 * anything.
	 */
	virtual void Disarm() override;

	/**
	 * ★ NET-MODE GATE, ADDED 2026-08-11. The handler cannot do anything on a joined client — it returns
	 * on `GetLocalRole() != ROLE_Authority`, and a joined client's own pawn is `ROLE_AutonomousProxy` —
	 * yet `Arm()` still installed a funchook detour on `SendClientAdjustment`, which the engine calls
	 * per movement correction. Pure overhead on the one machine that cannot benefit.
	 *
	 * Ant measured `FPM.Fix.StaticBase 0` restoring her movement on a dedicated-server client. This is
	 * the registration-time guard that makes that the default, and it is the same pattern
	 * `FFPMWwiseServerGate` already uses for the mirror-image case.
	 *
	 * It cannot live in `Arm()`: there is no world at startup, so the net mode is not knowable yet.
	 */
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.StaticBase.Report` — how many adjustments were rewritten, and how many were declined. */
	static void ReportNow();

private:
	/** The handle from Arm(), so Disarm() can remove exactly this handler and nothing else. */
	FDelegateHandle AdjustmentHookHandle;
};
