// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/FPMFixContract.h"

/**
 * ★ INVISIBLE-BUT-SOLID BUILDABLES — an incomplete CSS refactor, fixed at the component it broke.
 * Design P3.1. Board m6147739 (Tier 1, read from source in both trees).
 *
 * ═══ THE BUG, AND IT IS ONE LINE OF SEMANTICS ═══
 *
 * `ULightweightHierarchicalInstancedStaticMeshComponent` derived from HISM when it was introduced at
 * CL365306, and was changed to a plain ISM at CL480321 (2026-04-23). The game is CL495413, so this has
 * been live since April. The evidence that it was an incomplete CONVERSION rather than a decision is
 * still sitting in the header, one line above the declaration:
 *
 *     AbstractInstanceManager.h:261   //class ... : public UHierarchicalInstancedStaticMeshComponent
 *     AbstractInstanceManager.h:262   class   ... : public UInstancedStaticMeshComponent
 *
 * — the old declaration commented out, not deleted. Every local in `InstanceData.cpp` is still named
 * `Hism`.
 *
 * The mismatch that falls out of it:
 *   · `FInstanceComponentData::RemoveInstance` maintains its handle table with **RemoveAtSwap**
 *     (`AbstractInstanceManager.cpp:194-201`, and its own comment at `:166-175` says so), then calls
 *     `ISM->RemoveInstance(HandleId)` at `:203`.
 *   · **HISM always forced swap** (`HierarchicalInstancedStaticMesh.cpp:2206-2207`), so while it was a
 *     HISM the two agreed.
 *   · Plain `UInstancedStaticMeshComponent::RemoveInstance` uses **RemoveAt — a SHIFT DOWN**
 *     (`InstancedStaticMesh.cpp:3957-3959` -> `:3801-3809`).
 *
 * So from the second removal onward the handle table and the render data disagree about which index is
 * which, and the disagreement grows.
 *
 * ═══ WHY IT UNIQUELY EXPLAINS *SOLID AND INVISIBLE* ═══
 *
 * The collision side removes the LAST index after copying its transform into the hole
 * (`:237-240`), and last-index removal is identical under both semantics. So collision and `ResolveHit`
 * stay correct while only the visual binding rots — which is exactly the reported symptom, and is why
 * no other hypothesis fits it as cleanly.
 *
 * ⚠ IT FAILS SILENTLY BY CONSTRUCTION. The `HandleID == mesh index` invariant is only `EditorCheck`ed
 * (`:630`), compiled out of Shipping. Consistent with none of the 31 crash dumps landing in this path.
 *
 * ═══ THE FIX, AND WHY THIS FORM ═══
 *
 * The engine ships the escape hatch and nobody took it:
 *     InstancedStaticMeshComponent.h:459   void SetRemoveSwap() { bSupportRemoveAtSwap = true; }
 *     InstancedStaticMesh.cpp:3801         bUseRemoveAtSwap = bForceRemoveAtSwap || bSupportRemoveAtSwap
 *                                                             || CVarISMForceRemoveAtSwap...
 * `SetRemoveSwap()` has NO call site in either tree, and `r.InstancedStaticMeshes.ForceRemoveAtSwap`
 * defaults to 0.
 *
 * ★ PER-COMPONENT, NOT THE GLOBAL CVAR — and that is the whole reason this is a fix rather than a
 * one-line ini. The cvar is process-global: it would force swap semantics on EVERY ISM in the game,
 * including foliage and any mod's, and any caller that assumes stable indices across a removal breaks.
 * Board m6147739 flags exactly that ("RISK: the cvar is process-global, so watch machines and belts
 * too"). Scoping by CLASS reaches only the components with the mismatch.
 *
 * ⚠ ONLY THE MESH COMPONENT, NEVER `ULightweightCollisionComponent`. The collision twin
 * (`AbstractInstanceManager.h:251`) is already correct — see above — and changing its removal semantics
 * would break the half of this system that still works.
 *
 * ⚠ THIS FIX CHANGES NO GEOMETRY AND REMOVES NOTHING. It sets one bit that selects which removal
 * algorithm a component uses, so the repair only takes effect on removals AFTER it is armed. Instances
 * already mis-bound by earlier removals stay mis-bound until their component is rebuilt — a world
 * reload. That is stated rather than hidden, because "I armed the fix and the broken pieces are still
 * broken" is the obvious first report.
 *
 * ★ NO ACCESS TRANSFORMER. An earlier sketch reached `AAbstractInstanceManager::InstanceMap` through a
 * friend, as the distance-field audit does. It is not needed: the class is
 * `ABSTRACTINSTANCE_API`-exported and `SetRemoveSwap()` is public, so a class filter reaches exactly
 * the same set with no widened surface.
 */
class FICSITSPERFORMANCEMANAGER_API FFPMInstanceRemoveSwap final : public IFPMFix
{
public:
	static FFPMInstanceRemoveSwap& Get();

	virtual const TCHAR* Name() const override { return TEXT("instance-remove-swap"); }

	/**
	 * ⚠ `Any`, deliberately, even though the symptom is visual.
	 *
	 * The handle table this keeps consistent is not a render structure — `FInstanceComponentData` is
	 * what `ResolveHit` and the dismantle path resolve through. A dedicated server builds no render
	 * data but it DOES own the manager and its handles, and a server whose handle table drifts hands
	 * clients the wrong instance. Gating this to clients would fix the symptom on one machine and leave
	 * the authority disagreeing with it.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * OriginNamed, and unusually strongly: the cause is a specific changelist (CL480321), the two
	 * disagreeing call sites are read from source in both trees, and the engine's own opt-in is the
	 * repair. We are not holding a symptom down.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::InstanceSwap; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/**
	 * ★ OFF UNTIL THE A/B PASSES — Ant's ruling, 2026-08-11, on the day this shipped.
	 *
	 * This fix changes how buildable instances are REMOVED, on a live save, and it has never run once.
	 * The reasoning she picked: untested code changing removal semantics is exactly what bit her earlier
	 * the same day, when three fixes that reported themselves armed and healthy were doing nothing or
	 * doing harm. `DefaultArmed()` existed by then and off-by-default costs nothing.
	 *
	 * ⚠ THIS IS NOT DOUBT ABOUT THE DIAGNOSIS. The root cause is Tier 1, read from source in both trees
	 * (board m6147739), and the mismatch is not in question. What is untested is THIS CODE, on HER save.
	 *
	 * TO TURN IT ON: `FPM.Fix.InstanceRemoveSwap 1`. The A/B that promotes it to default-on is in
	 * m6147739 — count with `FPM.InstanceSwap.Enabled 0`, dismantle ten pieces watching the FIELD, then
	 * enable, RELOAD THE WORLD, and repeat. The reload is not optional: the flag only affects removals
	 * made after it arms, so instances already mis-bound stay mis-bound and would read as a failure.
	 */
	virtual bool DefaultArmed() const override { return false; }

	/** Sweeps existing components, then re-sweeps to catch ones created after the world loaded. */
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.InstanceSwap.Report` — components converted, already-correct, and sweeps run. */
	static void ReportNow();

private:
	/**
	 * One pass over the world. Returns how many components it had to CHANGE.
	 *
	 * ★ THAT RETURN VALUE IS THE EXPERIMENT, not bookkeeping. `bSupportRemoveAtSwap` is a plain
	 * `uint8 : 1` with NO `UPROPERTY` (`InstancedStaticMeshComponent.h:234`), so whether a value set on
	 * the CDO is inherited by components built later is UNDOCUMENTED — UE copies the archetype's memory
	 * during construction, which would carry it, but that is an implementation detail and not a
	 * contract. So the CDO is set as a cheap belt-and-braces and NOT relied on.
	 *
	 * The later sweeps then answer it for free: if new components arrive already set, this returns 0
	 * and the CDO route is proven. If it keeps returning non-zero, the CDO route does nothing and the
	 * sweeps are load-bearing. Either way the log says which, instead of the fix quietly depending on
	 * behaviour nobody checked.
	 */
	int32 SweepWorld(UWorld* World, const TCHAR* Moment);

	FTSTicker::FDelegateHandle ResweepHandle;
};
