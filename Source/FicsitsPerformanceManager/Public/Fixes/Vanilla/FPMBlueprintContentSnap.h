// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

class AFGBlueprintHologram;
class AFGBlueprintProxy;
class AFGBuildable;
class USceneComponent;
struct FHitResult;

/**
 * BLUEPRINT CONTENT SNAP — the two hooks. `FPMBlueprintContentSnapMode.h/.cpp` next to this file is the
 * INERT descriptor (a name, nothing else, design S6). This file is where the feature lives: design doc
 * `FPM-DESIGN-BLUEPRINT-FEATURE-SNAP-2026-08-02.md`, sections S5-S7.
 *
 * ★ WHAT ANT ASKED FOR (2026-08-02, verbatim in the design doc): *"there isnt a proper blueprint to
 * blueprint snapping... the blueprints cant find a good snap point becouse they try to snap to centre of
 * blueprints... it needs to be a snapping mode. we cant remove the other versions."* Vanilla's three
 * blueprint build modes only ever CONNECT two placed blueprints (`FGBlueprintOpenConnectionManager.h`
 * builds a bridge hologram, receipt 1.6 in the design doc); none of them ever MOVE the hologram being
 * placed to mate against the neighbour's actual contents. This fix is the missing mode.
 *
 * HOOK A — `AFGBlueprintHologram::GetSupportedBuildModes_Implementation` (measured 402-byte pad in the
 * shipped DLL, design S5). Calls through to vanilla, then appends our descriptor. Additive by
 * construction: an out-array appended to AFTER the original ran cannot lose vanilla's three modes.
 *
 * HOOK B — `AFGBlueprintHologram::TrySnapToActor` (measured 9,895-byte pad — the largest placement-path
 * target in the design's survey, S5). Calls through to vanilla FIRST — vanilla's own box/centre proxy
 * snap still runs and still wins outside our mode. Only when `IsCurrentBuildMode` names OUR descriptor
 * does this fix look for a better answer; if it does not find one, vanilla's result is left untouched.
 *
 * ★ V1 SCOPE, STATED RATHER THAN HIDDEN. Design S3 accepts two anchor kinds: A1 (connection components —
 * belt/pipe/rail) and A2 (a foundation lattice, for buildables with no connections at all). Design S12 Q3
 * leaves the v1 scope of A2 an OPEN QUESTION FOR ANT, unruled as of this build. A2 needs footprint fields
 * this build never read (S3: "exact field names to be lifted from those headers at build time... HYPOTHESIS
 * until read") plus an LRU anchor cache and hysteresis lock (S7 par.4, S9) that do not exist anywhere in
 * this project yet. Building all of that unruled and unread is exactly the kind of thing LAW 17 says is
 * hers to call, not picked silently — so THIS BUILD SHIPS A1 ONLY. A blueprint whose only contents are
 * connection-less foundation pieces (a road pack with no connectors) will find no candidate here and fall
 * back to vanilla free placement, same as today. That is a real, named limitation, not a bug.
 *
 * ★ THE OUR-SIDE ACCESS TRANSFORMER THE DESIGN DOC ASKED FOR TURNED OUT UNNECESSARY. S3 assumed
 * `mBuildableToNewRoot` needed a friend to read, alongside the private `mOpenConnectionManagers`. Reading
 * `FGBlueprintHologram.h` for this build found `mBuildableToNewRoot` sits in the PUBLIC section (before
 * the first `protected:`) — it is a plain public UPROPERTY. Our own open connections are enumerated by
 * walking its keys (the real `AFGBuildable` actors loaded into the hologram's private preview world,
 * `LoadBlueprintToOtherWorld`) and calling `GetComponents<T>()` on each, exactly like vanilla's own
 * `RegisterNearbyActor` (design receipt 1.7) — no `AccessTransformers.ini` entry, no friend class.
 * `mOpenConnectionManagers` itself is never touched.
 *
 * ★ COMPATIBILITY REUSES VANILLA'S OWN RULE, NOT A GUESSED ONE. `UFGFactoryConnectionComponent::CanSnapTo`
 * and `UFGPipeConnectionComponentBase::CanSnapTo` (both public, both virtual) ARE vanilla's own "would
 * these two connectors snap" test; rail has no `CanSnapTo`, so `UFGRailroadTrackConnectionComponent::
 * CanConnectTo` (public) is used instead. Direction, fluid type, and every other vanilla precondition are
 * vanilla's problem to get right, not re-derived here.
 *
 * ⚠ THE GEOMETRY IS EXISTENCE-PROVEN, NOT EXECUTION-PROVEN. `mBuildableToNewRoot`'s doc comment says the
 * mapped `USceneComponent` "represents [the buildable] visually in the game world" — this build reads
 * that as "the connector's pose relative to its owning buildable, reapplied onto that scene component's
 * CURRENT world transform, is where the connector sits right now." That reading compiles and is the only
 * one the header supports, but it has not been watched on a boot. Boot-verification checklist below.
 *
 * ★ THE MIRROR CHECK: WHAT CORRECT INPUT MIGHT THIS WRONGLY REJECT? A geometrically perfect pairing is
 * rejected only when (a) `CanSnapTo`/`CanConnectTo` itself says no — that is vanilla's call, not a false
 * rejection by this fix — or (b) the connector is currently `IsConnected()` — correct, an already-wired
 * connector is not open to the outside. Neither is a bug in this fix. The one this fix COULD get wrong is
 * the geometry assumption above: if a buildable's local rotation inside the blueprint is not carried
 * faithfully by its surrogate scene component, a genuinely mateable pair would compute a wrong transform
 * rather than being silently skipped — which is why `FPM.BlueprintContentSnap.BudgetMs` and the report
 * below exist: a wrong-looking snap is something Ant can SEE and report, not a silent miss.
 *
 * NO NETWORK, NO ASSET, NO CVAR ON A `US_*` SETTING, ZERO RESIDUE (design S10, S6): the descriptor is a
 * native class carrying only a display string, both hooks are additive, and nothing here ever dismantles,
 * deletes, or replaces a placed blueprint — it only repositions a HOLOGRAM (a preview, never a real
 * object) before construction. Ant's blueprint rule ("never delete any blueprint on the live server,
 * ever") cannot be touched by this fix by construction: it has no delete/dismantle path at all.
 *
 * BOOT-VERIFICATION CHECKLIST (design S11 — none of these can be settled from headers):
 *   1. Vanilla's own proxy/box snap does not fight ours when our mode is current (design S11.1).
 *   2. `AreProxyBuildingsRegisteredAndValid()` gate actually degrades to vanilla on a client mid-replicate.
 *   3. The geometry assumption above — does a mated placement actually look mated, not offset/twisted.
 *   4. Server-side construction accepts a mated (coincident-connector) transform like any direct-connect
 *      placement (design S11.4) — this fix computes client-side only; nothing new crosses the wire.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMBlueprintContentSnap final : public IFPMFix
{
public:
	static FFPMBlueprintContentSnap& Get();

	virtual const TCHAR* Name() const override { return TEXT("blueprint-content-snap"); }

	/**
	 * `Any`, per `FPMFixContract.h`'s stated bias and design S8. The hooked functions only ever run on an
	 * ACTOR ACTUALLY PLACING A HOLOGRAM, which requires a local aiming player — a dedicated server has
	 * none, so the hooks are armed everywhere but structurally never fire there. Design S8: "the hooks are
	 * armed but inert" on the server. The descriptor class exists on both builds regardless (needed so the
	 * replicated `mBuildModeOverride` class path resolves on both ends).
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * IMPERFECT FIT, STATED RATHER THAN HIDDEN — same call `FFPMWristSlotHook` made and said out loud
	 * (`FPMWristSlotComponent.h`). `EFPMOriginStatus`'s four values are all about a DEFECT's evidence
	 * tier; this hook provisions a new capability rather than fixing a named one. `Guard` is the
	 * least-wrong available value: its job is structural/additive — offer a mode, mate connectors when
	 * asked — and it changes nothing about vanilla's own three build modes or free placement outside it.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::BlueprintContentSnap; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.BlueprintContentSnap.Report` — the liveness counters, printed with what each one proves. */
	void LogReport();

private:
	/**
	 * The whole placement algorithm, design S7. Returns false (leaving vanilla's result untouched) when
	 * no target proxy, no registered/valid proxy, or no compatible open-connector pair is found.
	 */
	bool TryComputeContentSnap(AFGBlueprintHologram* Self, const FHitResult& Hit, FTransform& OutTransform);

	FDelegateHandle GetSupportedBuildModesHandle;
	FDelegateHandle TrySnapToActorHandle;

	/*
	 * EVERY COUNTER HERE HAS A NAMED NON-ZERO CONDITION — the dead-instrument check.
	 *   ModeOffered    -> non-zero the first time ANY blueprint hologram asks for its supported modes
	 *                      (Hook A fires on every such query, in or out of our mode; a fresh boot moves
	 *                      this to 1 the moment a player picks up a blueprint).
	 *   InModeFrames   -> non-zero only once a player has actually CYCLED to this mode and is aiming.
	 *   SnapsApplied   -> non-zero only once a compatible open-connector pair was actually found and a
	 *                      transform actually written to the hologram. THE feature-works proof.
	 *   NoProxyFrames  -> non-zero when aiming in-mode at something that resolves to no proxy, or an
	 *                      unregistered one — expected during normal free-aim, not itself a fault.
	 *   NoCandidateFrames -> non-zero when a proxy was found but nothing on it had an open, compatible
	 *                      connector — expected for a blueprint with no A1 contents (the v1 boundary).
	 *   BudgetExceeded -> MUST stay 0 on a small test blueprint and is the honest admission that v1 has
	 *                      no anchor cache (design S9); a large blueprint is expected to move this.
	 */
	int32 ModeOffered = 0;
	int32 InModeFrames = 0;
	int32 SnapsApplied = 0;
	int32 NoProxyFrames = 0;
	int32 NoCandidateFrames = 0;
	int32 BudgetExceeded = 0;
};
