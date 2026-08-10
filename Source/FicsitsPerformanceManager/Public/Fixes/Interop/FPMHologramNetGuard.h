// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * HOLOGRAM NET REPAIR — a joining client used to die on someone else's build preview.
 *
 * THE CRASH (SunFry, message.txt 2026-07-21 — she cannot join, singleplayer is fine):
 *   Assertion failed: mCachedAttachmentPoints.Num() > 0
 *     ModularStations\FGCarouselHologram.cpp:55
 *   AFGCarouselHologram::BeginPlay()  <-  AActor::DispatchBeginPlay()
 *     <- UActorChannel::ProcessBunchInternal()      (i.e. arriving over the network)
 *
 * A build hologram is the placement PREVIEW ghost. In multiplayer the server replicates it so other
 * players can see what someone is placing. The crash lands DURING join, which is the mechanism that
 * rebinds SunFry to a fresh character and wipes her inventory — rescued twice. So this is not a
 * cosmetic bug.
 *
 * ★ WHY THIS IS A REPAIR AND NO LONGER A SKIP. THE OLD FIX HAD A REAL, OBSERVED COST.
 *
 * The old mod cancelled AActor::DispatchBeginPlay outright for every ROLE_SimulatedProxy hologram —
 * 475 of them a session, the overwhelming majority vanilla. The 2026-08-08 triage flagged the cost as
 * a HYPOTHESIS, because FactoryGame's BeginPlay bodies are stubbed in the SML tree and nobody had
 * seen the consequence. ANT HAS SEEN IT: *"i actually HAVE seen this. sometimes the holograms never
 * rendered when sunfry held them in front of me."* That promotes it to a MEASUREMENT, and it is the
 * reason this file exists in this shape.
 *
 * The mechanism is not subtle once the cost is known. DispatchBeginPlay does not merely call the
 * actor's BeginPlay — it dispatches BeginPlay to every COMPONENT and sets the actor's begun-play
 * state. Cancelling it leaves the ghost's mesh components un-begun, so the preview never appears.
 * One over-broad cancel, two symptoms: no crash, and no ghost.
 *
 * Ant's instruction, 2026-08-08: *"we dont cut ANY feature here, just fix the core issue and keep 100%
 * features complete."*
 *
 * ★ THE CORE ISSUE. `mCachedAttachmentPoints` is VANILLA, not ModularStations —
 * FGBuildableHologram.h:523, a plain TArray with no UPROPERTY, therefore never replicated. The points
 * are derived locally during the placement flow, which a network-received observer copy never runs.
 *
 * WHERE THE DATA ACTUALLY LIVES — SETTLED FROM ASSET BYTES, AFTER GETTING IT WRONG ONCE. Of 400
 * exported assets containing `FGAttachmentPointComponent`, 350 are `Deco_*` DECORATION TEMPLATES and
 * nearly all the rest are decorators as well. Vanilla assembles a hologram's points in
 * `AFGBuildable::CreateAttachmentPointsFromComponents` (FGBuildable.cpp:2539-2566, real code) from the
 * BUILDABLE's decoration template plus the buildable's own components — the hologram is passed only as
 * `owner`, to filter by usage. Nothing reads components off the hologram actor.
 *
 * ⚠ v0.2.0 DID READ THE HOLOGRAM'S COMPONENTS, AND WOULD HAVE FOUND NOTHING. Every fire would have
 * fallen through to the residual cancel below, shipping the regression this fix removes while claiming
 * "Expected: zero". Caught in review before it ever booted. It is the RAIN MISTAKE EXACTLY — right
 * intent, right hook, wrong object — made in a file whose own comment cited rain as the lesson.
 *
 * So the repair calls vanilla's routine on the CDO of the replicated `mBuildClass` (UPROPERTY(Replicated),
 * FGHologram.h:756) and lets BeginPlay run as intended. Reading the CDO also sidesteps the timing
 * problem: a CDO's subobjects exist before any BeginPlay, whereas the hologram's own components are
 * copied in by SetupComponents DURING BeginPlay (FGHologram.h:633-636).
 *
 * ⚠ WHY POPULATING A PROXY'S CACHE IS GAMEPLAY-INERT. A simulated proxy never makes a placement
 * decision — snapping is resolved on the machine that owns the hologram and committed by the server.
 * The cache on an observer copy is read only by code that expects it to be populated. Filling it
 * changes what that code sees and nothing else; no build, snap or clearance outcome depends on it.
 *
 * ⚠ THE RESIDUAL CANCEL, AND WHY IT SURVIVES. If a hologram has no usable attachment-point components
 * at all, the repair cannot produce a point and the assert would still fire — killing the joiner and
 * costing an inventory. Skipping a preview is cosmetic; a join crash is not. So the cancel remains, but
 * ONLY for that case, and it logs the class name every time so the next boot can say how large the
 * residual set actually is. Expected: zero. If a VANILLA class ever appears in that bucket, the cancel
 * is too wide again and must be narrowed to non-FactoryGame classes — that is the next move, and it is
 * deliberately not pre-emptive, because guessing at this is what produced the 475-per-session cancel.
 */
class FFPMHologramNetGuard final : public IFPMFix
{
public:
	static FFPMHologramNetGuard& Get();

	virtual const TCHAR* Name() const override { return TEXT("hologram-net"); }

	/*
	 * ⚠ THE SIDE IS RIGHT; THE FIRST REASON GIVEN FOR IT WAS NOT.
	 *
	 * It said "a build preview is renderer work and a dedicated server draws nothing". The fix contract
	 * rejects that shape of argument explicitly — `NeverOnDedicatedServer` is not justified by "this
	 * feels like a client thing", and holograms are gameplay objects the server has authority over, not
	 * purely renderer state.
	 *
	 * THE ACTUAL REASON IS A COST ONE. The hook target is `AActor::DispatchBeginPlay`, which runs for
	 * EVERY actor in the game. The handler's first condition is `GetLocalRole() == ROLE_SimulatedProxy`,
	 * and a dedicated server never holds a hologram as a simulated proxy — it is the authority for the
	 * ones it replicates out. So on a server this hook could fire on every actor's BeginPlay and could
	 * never once reach its body. Skipping it there removes a universal hook with no reachable benefit;
	 * that is a measurable saving, not a preference.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** ChokePointRepair: vanilla derives mCachedAttachmentPoints locally and never replicates it, and the modded proxy's
	 * cacheless arrival is named for ModularStations -- close to OriginNamed, deliberately NOT promoted
	 * until the carousel-class mechanism carries a receipt. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::ChokePointRepair; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::HologramNet; }

	virtual void Arm() override;

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
	FDelegateHandle DispatchBeginPlayHandle;
};
