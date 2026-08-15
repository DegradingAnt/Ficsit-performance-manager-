// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * RAIN OCCLUSION REPAIR — gives buildables the occlusion box their author never filled in.
 *
 * THE BUG. AFGBuildable's constructor zeroes mRainOcclusionBoundingBox and sets IsValid=false
 * (FGBuildable.cpp:137-143). A buildable whose author never filled it in therefore participates in the
 * rain system with an invalid box, vanilla logs "0 Volume rain occlusion box found! ... IsValid=false",
 * and rain falls straight through the geometry. Ant, 2026-08-07: "the rain issue still happens. rain
 * goes through alot of geometry still."
 *
 * ★ WHY THE OLD FIX DID NOT WORK, MEASURED 2026-08-08. It was not missing classes — it was FAILING on
 * the classes it reached. The client log shows the repair running on a class and that exact class
 * erroring ONE MILLISECOND later:
 *     20:00:58.059  rain occlusion fix (lightweight path): 1 class(es) repaired ... Build_Concrete_Dome_Ceiling_8x8_C
 *     20:00:58.060  LogRainSystem: Error: 0 Volume rain occlusion box found! ... Build_Concrete_Dome_Ceiling_8x8_C
 * The old collector derived bounds by walking COMPONENTS for a UStaticMeshComponent with a mesh. For
 * this population there is no such mesh. Its FModel export shows the class's only mesh-bearing
 * component, FGColoredInstanceMeshProxy 'BuildingMeshProxy', serialising just three properties —
 * BodyInstance, AttachParent, RelativeLocation — and NO StaticMesh at all. GetStaticMesh() returns
 * null, bounds come back invalid, the repair takes its else-branch and never writes a box.
 *
 * ★ WHERE THE GEOMETRY ACTUALLY LIVES. On the AbstractInstance data object:
 *     mCanContainLightweightInstances = true
 *     mInstanceDataCDO -> UAbstractInstanceDataObject
 *         Instances[] = { StaticMesh, RelativeTransform, CollisionProfileName, Mobility, ... }
 * All public API: FGBuildable.h:823 GetLightweightInstanceData(), InstanceData.h:305/310/313. No
 * AccessTransformer needed. The old collector was reading the wrong object, and no filter tweak would
 * ever have fixed that.
 *
 * ★ THE COLLISION FILTER IS ANT'S, AND IT IS WHAT MAKES THIS CORRECT rather than merely working —
 * "everything has collision. why not just make that do the rain interaction". Collision GEOMETRY in UE
 * comes from the static mesh's body setup, so the mesh IS the collision; CollisionProfileName is the
 * response. Filtering on it drops the snap dummies, trigger volumes and clearance helpers that have a
 * mesh but should never stop rain — the false-positive class the mesh-only approach had no way to see.
 *
 * BOTH SOURCES ARE KEPT, because the data says both are needed: the component walk demonstrably
 * repaired 1,300-2,700 buildables per session, so it works for most classes; the instance data is what
 * the failing ones use. Instance data is tried first and the component walk is the fallback.
 *
 * SIDE: NEVER ON A DEDICATED SERVER, and this is measured rather than assumed. Across 11 DatHost
 * session logs (92,092,113 bytes) that demonstrably cover startup, "LogRainSystem" appears ZERO times,
 * while the client logs 28-35 of these errors every session. The server was nonetheless repairing up to
 * 2,700 buildables per session — rendering data for a machine with no renderer. mRainOcclusionBoundingBox
 * is EditDefaultsOnly (FGBuildable.h:519-520), neither Replicated nor SaveGame, so nothing it computes
 * can reach a client either. Skipping the server here GIVES CPU BACK; it does not cost a feature.
 */
class FFPMRainOcclusionFix final : public IFPMFix
{
public:
	static FFPMRainOcclusionFix& Get();

	virtual const TCHAR* Name() const override { return TEXT("rain-occlusion"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** OriginNamed: classes ship with an unauthored mRainOcclusionBoundingBox and their real geometry lives on
	 * mInstanceDataCDO -- both read from FModel bytes, so the cause is receipted. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::Rain; }
	virtual void Arm() override;

	/**
	 * ★ THE SWEEP, AND IT IS THE POINT OF THIS FIX RATHER THAN AN OPTIMISATION.
	 *
	 * Ant, 2026-08-08: "why cant we fix the rain issue offline and just save that forever? why does it
	 * need to do it live at all?" — then "we could just derive it at runtime ONCE per system when in a
	 * loading screen or the benchmark screen. problem solved. also solves adding new mods."
	 *
	 * She is right, and the old fix's reactive shape was the actual mistake. The box is per-class
	 * CONSTANT data: same inputs every session, same answer. Deriving it lazily meant thousands of
	 * per-object hook firings during play, and a class whose first instance never spawned simply never
	 * got repaired.
	 *
	 * This runs once, in the CONSTRUCTION phase of a save load, with the loading screen up. The hooks
	 * stay armed but demote to a MISS-HANDLER for classes the sweep could not see — a blueprint that
	 * loads later in the session, or a mod added mid-session. Deriving on the machine rather than
	 * shipping a table also means we never distribute measurements of anyone else's geometry.
	 */
	virtual void OnWorldLoad(UWorld* World) override;

	/**
	 * Removes all 3 hooks.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is where DisarmAll was called from until P4.2
	 * shipped the master OFF switch (`FPM.Enabled 0`, `FPMMasterSwitch.cpp`) - that is why the
	 * omission survived that long. DisarmAll now also runs mid-session from that switch, which is
	 * exactly why this override has to be correct.
	 */
	virtual void Disarm() override;

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle BuildableBeginPlayHandle;
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle AddShapeFromClassHandle;
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle AddShapeFromBuildableHandle;
};
