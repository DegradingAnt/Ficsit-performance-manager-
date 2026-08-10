// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * DO THE PLAYER'S BUILDINGS HAVE DISTANCE FIELDS? COUNT THEM, DO NOT ASSUME.
 *
 * ★ ONE SUSPECTED CAUSE UNDER THREE OF ANT'S SYMPTOMS, which is why this is worth a whole fix:
 *
 *   - rain passes through built walls, while `NS_Rain` demonstrably owns a `QueryMeshDistanceFieldGPU`
 *     collision query. If the query is there and the wall is not in the distance field, the rain goes
 *     through it and the collision module is innocent.
 *   - *"light needs to not go through terrain and buildables like it does on low settings"* (Ant,
 *     2026-08-10).
 *   - distance-field shadow pop-in, parked at `m6333007` after virtual shadow maps were measured too
 *     expensive to use instead.
 *
 * A single mechanism would explain all three, and it is cheap to test. That convergence is the argument
 * for looking here first rather than building three fixes.
 *
 * ★ THE MECHANISM, READ FROM ABSTRACTINSTANCE'S OWN SOURCE.
 *
 * Lightweight buildables — every ordinary foundation and wall since 1.0 — are instanced meshes owned by
 * `AAbstractInstanceManager`, and their distance-field contribution is switched OFF while the world
 * lazy-loads:
 *
 *     AbstractInstanceManager.cpp:827   bEnableDistanceFieldShadows = !bIsLazyLoading
 *                                                                     && InstanceData.bCastDistanceFieldShadows
 *     AbstractInstanceManager.cpp:305   newMeshComp->bAffectDistanceFieldLighting = bCastDistanceFieldShadows;
 *                                       // "distance fields are enabled after lazy loading."
 *
 * and switched back on by a ONE-SHOT pass inside `AAbstractInstanceManager::Tick` (`:433`), which fires
 * on the single tick where both lazy queues happen to be empty:
 *
 *     AbstractInstanceManager.cpp:482   if ( PriorityLazyLoadTasks.IsEmpty() && LazyLoadTasks.IsEmpty() )
 *     AbstractInstanceManager.cpp:493       MeshComp->bAffectDistanceFieldLighting = true;
 *                                           MeshComp->MarkRenderStateDirty();
 *     AbstractInstanceManager.cpp:498       bAllowLazySpawn = false;
 *
 * The default intent is ON — `InstanceData.h:67` has `bCastDistanceFieldShadows = true`. So any
 * component still sitting at `false` after loading has MISSED that pass, and is invisible to every
 * distance-field consumer in the renderer for the rest of the session.
 *
 * ⚠ WHETHER THAT ACTUALLY HAPPENS IN ANT'S WORLD IS EXACTLY WHAT IS UNKNOWN, and reading more of
 * vanilla's code cannot settle it — the answer depends on streaming order in a specific save. So this
 * counts. A count of zero kills the theory outright and is a good result; a count above zero names the
 * bug and the meshes it affects.
 *
 * ★ IT AUDITS. IT DOES NOT REPAIR UNLESS ASKED, AND THAT IS A PERFORMANCE DECISION.
 *
 * Setting `bAffectDistanceFieldLighting` back to true is one line, and doing it to every instanced mesh
 * in a large factory is NOT free — each one starts contributing to distance-field lighting and shadow
 * work that it currently skips. Ant: *"Keep the performance good."* A performance mod that fixes a
 * visual by quietly adding renderer work has failed at its own job.
 *
 * So the repair is behind `FPM.DistanceField.Repair`, default **0**, and when it does run it reports
 * how many components it touched so the cost has a number attached rather than a shrug.
 *
 * VIEWER BY DEFAULT: it reads component flags and prints. No hook, no console-variable write, no ini.
 */
class FFPMDistanceFieldAudit final : public IFPMFix
{
public:
	static FFPMDistanceFieldAudit& Get();

	virtual const TCHAR* Name() const override { return TEXT("distance-field-audit"); }

	/**
	 * ⚠ CLIENT ONLY, AND FOR A REASON WORTH STATING: distance fields are a RENDERER structure. A
	 * dedicated server builds none, so every component there would read false and the audit would
	 * report a catastrophe that is simply the absence of a renderer. That is the "instrument reports an
	 * absence as a finding" trap this project keeps paying for, so the side gate closes it.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** UnknownCause: the theory is receipted from AbstractInstance's source, the OCCURRENCE is not. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::DistanceField; }

	virtual void Arm() override;

	/** Audits after the world has had time to finish lazy-loading. */
	virtual void OnWorldLoad(UWorld* World) override;

	/** `FPM.DistanceField.Audit` — count now and report. */
	static void AuditNow();
};
