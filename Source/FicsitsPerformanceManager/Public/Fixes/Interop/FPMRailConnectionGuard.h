// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ RAIL CONNECTION GUARD — stops a server-killing assert when a blueprint containing track is placed.
 * Design P3.2.
 *
 * THE CRASH (dedicated server, first seen 2026-07-22, placing a BLUEPRINT containing railway track):
 *
 *     Assertion failed: GetTrack()->GetConnection( 1 ) == this
 *       FGRailroadTrackConnectionComponent.cpp
 *     UFGRailroadTrackConnectionComponent::GetOpposite()
 *       <- DynamicTrainRoutes: ADynamicTrainPathfindingSubsystem::AddNode(...)
 *       <- UActorComponent::RegisterComponentWithWorld
 *       <- AFGBuildableHologram::SetupComponent
 *       <- AFGBlueprintHologram::DuplicateConnectionComponent<UFGRailroadTrackConnectionComponent>
 *
 * A blueprint HOLOGRAM duplicates a rail connection component for its preview. The instant that
 * component registers with the world, DynamicTrainRoutes grabs it as a pathfinding node and calls
 * `GetOpposite()`. A hologram's duplicated connection is not wired into real track — its track either
 * does not exist yet or does not list this component — so vanilla's invariant fires and takes the
 * server down. Same class as the carousel hologram bug: a mod treating a build PREVIEW as a placed
 * building.
 *
 * ★ HOW OFTEN, MEASURED — this is not a rare edge case.
 * Across the 12-log DatHost corpus (2026-08-06 → 08-07), the old mod's version of this guard fired
 * **1,900 to 2,550 times per server start, in every one of the 11 real sessions — 23,450 averted
 * asserts in total.** They arrive in a burst at startup: counters #50 through #950 land inside 26 ms.
 * Every one of those is an assert that would otherwise have killed the server.
 *
 * ★ THE GUARD: REPRODUCE VANILLA'S OWN PRECONDITION, THEN DECLINE INSTEAD OF ASSERTING.
 * `GetOpposite()` is only meaningful for a connection actually attached to a track. Vanilla asserts
 * `GetTrack()->GetConnection(1) == this`, which requires a track at all AND that this component is one
 * of that track's connections. When that does not hold we return `nullptr` — "there is no opposite" is
 * the honest answer for a preview component, and it is what the caller would have to handle anyway.
 *
 * ⚠ DELIBERATELY NARROW, AND THAT IS WHAT MAKES IT SAFE. A real, properly-wired connection satisfies
 * the precondition and is forwarded to vanilla untouched, so live rail pathfinding is bit-for-bit
 * unchanged. The guard can only ever fire where vanilla was going to abort the process.
 *
 * It is also GENERIC rather than DynamicTrainRoutes-specific: any mod that walks a hologram's rail
 * connections is covered. Naming one mod in the log is attribution, not the filter.
 *
 * ★ ORIGIN: NOT OURS, AND THE UPSTREAM REPORT IS THE REAL FIX (design G3/I14).
 * The cause is DynamicTrainRoutes treating a hologram connection as placed track. This guard holds the
 * door shut; it does not repair the caller. P3.2's origin work is the report to that author, and the
 * numbers above are what makes it a report rather than a complaint.
 */
class FFPMRailConnectionGuard final : public IFPMFix
{
public:
	static FFPMRailConnectionGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("rail-connection-guard"); }

	/*
	 * `Any`, and emphatically so: the observed crash is on the DEDICATED SERVER, which is where blueprint
	 * placement is authoritative. A client-only gate would leave the guard off exactly the machine that
	 * dies.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** Guard: the cause belongs to another mod's pathfinding, and the assert belongs to CSS. Neither is
	 *  ours to own — we prevent the harm and the upstream report chases the cause. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::RailGuard; }

	virtual void Arm() override;

	/**
	 * §3.3's denominator pair: asserts AVERTED, and calls PASSED THROUGH untouched.
	 *
	 * Both numbers, never just the first. "2,100 averted" alone cannot distinguish a guard doing vital
	 * work from a guard that has become over-broad and is refusing real connections — the pass-through
	 * count is what makes the averted count readable, and a guard whose averted count falls to zero
	 * after the upstream fix lands is a guard we can then retire on evidence.
	 */
	static void GetCounts(int32& OutAverted, int32& OutPassedThrough);

	/**
	 * Removes the hook.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is the only place DisarmAll has ever been called
	 * from and why the omission survived; it is what blocked P4.2's master OFF switch.
	 */
	virtual void Disarm() override;

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle GetOppositeHandle;
};
