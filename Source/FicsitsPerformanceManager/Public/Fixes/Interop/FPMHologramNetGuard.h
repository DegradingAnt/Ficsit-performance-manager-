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
 * are derived locally during the placement flow, which a network-received observer copy never runs. So
 * the ghost arrives with the components that describe its attachment points, and an empty cache
 * summarising them.
 *
 * THE DATA IS ALREADY THERE. `UFGAttachmentPointComponent::CreateAttachmentPoint(AActor*)` is public
 * and FACTORYGAME_API (FGAttachmentPointComponent.h:28); the components themselves are ordinary scene
 * components on the received actor. So the repair is to rebuild the cache from them and then let
 * BeginPlay run exactly as vanilla intended. Same lesson the rain fix was rebuilt on: the old code
 * failed because it read the wrong place, not because the data was missing.
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
	 * A build preview is renderer work and a dedicated server draws nothing, so no hologram is ever
	 * replicated TO a server. The old mod reached the same side by an early return inside the handler;
	 * the contract expresses it once, at arm time, where it can be read.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	virtual void Arm() override;
};
