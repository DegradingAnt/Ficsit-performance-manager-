// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ M-DETECT TRAP 2 - "CUSTOM MASTER MATERIALS THAT SKIP THE VANILLA BELT MASTER" (design §9.3,
 * item 2). A mod's conveyor-item material that does not derive from the same root material as
 * vanilla's belt items loses CSS's at-scale conveyor rendering optimisation (GPU instancing keyed
 * off a shared master).
 *
 * ★ THE ACCESSOR, VERIFIED RATHER THAN REFLECTED. `UFGItemDescriptor::GetItemMesh()`
 * (`FGItemDescriptor.cpp:56-61`) is a public static returning `mConveyorMesh` - a `protected`
 * field with no other public getter - so this is a real, direct API call, not a reflection
 * workaround. "Belt mesh material" is that mesh's own material slots
 * (`UStaticMesh::GetStaticMaterials()`, `Engine/StaticMesh.h:985`), and each slot's ROOT material
 * is `MaterialInterface->GetMaterial()`, which walks a material-instance chain to its base.
 *
 * ★ WHY THE "VANILLA MASTER SET" IS DERIVED, NOT HARDCODED. The trap names a specific asset
 * (`MM_FactoryBaked`) but that string is NOT visible anywhere in FactoryGame's own C++ source -
 * grepped the whole module for `MM_FactoryBaked` and `FactoryBaked`, zero hits - so hardcoding it
 * would be a guess, which this project's own doctrine forbids ("call the proper libs... never a
 * guessed [name]"). Instead: every conveyor-mesh material used by an item descriptor under the
 * VERIFIED vanilla content mount (`/Game/FactoryGame/…` - the same prefix
 * `FPMAssetResidency.cpp:28-54` already relies on) is collected into the "vanilla master set" at
 * runtime. That is empirically what CSS's own shipped items use, self-updating across a game
 * patch that renames the asset - more robust than a literal string, not less precise.
 *
 * ★ LOADED-ONLY, STATED AS A LIMIT RATHER THAN HIDDEN (design's own phrase, §9.3 item 2): a full
 * scan of every item descriptor in the game would force-load content that is not otherwise
 * needed, which is exactly the sync-load cost this mod exists to avoid (`m5660227`/`m6263134`
 * territory). This walks only classes already resident.
 *
 * ★ THE LIVENESS QUESTION: WHAT CONCRETE INPUT MAKES THIS REPORT NON-ZERO? Any loaded, non-vanilla
 * `UFGItemDescriptor` subclass whose conveyor mesh carries a material slot whose root material is
 * NOT a member of the vanilla master set collected this same run. Self-test proves the mechanism
 * against real, always-loaded vanilla content (see `SelfTest`'s own comment) rather than a
 * synthetic case.
 *
 * VIEWER ONLY: reads class reflection data and static-mesh material slots, reports through
 * FFPMDetectorRegistry. No hook, no cvar write, no ini.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMMasterMaterialDetector final : public IFPMFix
{
public:
	static FFPMMasterMaterialDetector& Get();

	virtual const TCHAR* Name() const override { return TEXT("master-material-detector"); }

	/** Any - reads static-mesh/material data that exists identically on a dedicated server (no
	 *  renderer needed to inspect an asset's material graph, only to draw it). */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Detect; }

	virtual void Arm() override;
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.Detect.MasterMaterial` - runs the census now. Needs no world (item descriptors are
	 *  loaded content, not actors), but is bound to world load for the same "run after the save
	 *  has had time to load its content" reasoning every census in this file uses. */
	static void RunNow();

	/**
	 * ★ THE LIVENESS PROOF. Builds the vanilla master set from `/Game/FactoryGame/` item
	 * descriptors currently loaded - which, on ANY running instance of this game, is never empty:
	 * the vanilla conveyor belt items (iron ore, the starting recipes) are loaded before a save
	 * even finishes opening. Known-positive: the set must be non-empty. Known-negative: a vanilla
	 * item's own material must classify as a MEMBER of the set it was built from (a set that
	 * excludes its own contents would flag the whole vanilla game as the trap, which would be the
	 * loudest possible false positive this detector could produce).
	 *
	 * @return true if both checks passed.
	 */
	static bool SelfTest();
};
