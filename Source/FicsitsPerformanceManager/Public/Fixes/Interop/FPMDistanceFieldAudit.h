// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

class UInstancedStaticMeshComponent;

/**
 * Where a component's distance-field state CAME FROM, which is the difference between a content
 * decision and a defect. See the class comment for why one number could not tell them apart.
 */
enum class EFPMDfProvenance : uint8
{
	/** Not owned by any AAbstractInstanceManager. Not judged, never repaired. */
	Foreign = 0,
	/** `InstanceData.bCastDistanceFieldShadows == false`. Authored off on purpose. Leave it. */
	AuthoredOff,
	/** Authored true, stored false — lazy loading poisoned it. Expected to be 0 on this build. */
	LazyPoisoned,
	/** Authored true, stored true. If the renderer flag is still clear, that is the real defect. */
	ShouldContribute,
};

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
 * ⚠⚠ THE LAZY-LOAD MECHANISM THIS FILE ORIGINALLY BLAMED IS SWITCHED OFF, AND EVERY VERSION BEFORE
 * 2026-08-10 ASSERTED IT ANYWAY — IN THE LOG, TO ANT, ONCE PER SAMPLE.
 *
 * The story was: lightweight buildables get distance fields disabled during lazy load
 * (`AbstractInstanceManager.cpp:305`, `:827`) and re-enabled by a one-shot pass when the queues drain
 * (`:488-499`), so anything still `false` afterwards MISSED that pass. It is coherent, the line numbers
 * are real, and there is even a genuine self-defeating bug inside it — the re-enable at `:489` is gated
 * on `entryPair.Value.bCastDistanceFieldShadows`, the very field lazy loading forces to false, so that
 * pass provably cannot repair what it exists to repair.
 *
 * **None of it executes.** The gate is a console variable that ships OFF:
 *
 *     AbstractInstanceManager.cpp        TAutoConsoleVariable<int32> CVarAllowLazySpawning(
 *                                            TEXT("lightweightinstances.AllowLazySpawn"), 0,
 *                                            TEXT("... (Temp disabled.)"), ECVF_Default );
 *     AbstractInstanceManager.cpp:792    bAllowLazySpawn = CVarAllowLazySpawning.GetValueOnGameThread() == 1;
 *
 * Verified not overridden anywhere in the cooked config, searched with `r.Nanite.Streaming.
 * StreamingPoolSize=50` (`DefaultEngine.ini:47`) as the known-positive liveness test — so that zero is
 * a measurement and not a broken grep. With the cvar at 0, `bIsLazyLoading` is always false and `:827`
 * reduces to `bEnableDistanceFieldShadows = InstanceData.bCastDistanceFieldShadows`.
 *
 * ★ SO A COMPONENT SITTING AT `false` IS ALMOST CERTAINLY AUTHORED THAT WAY.
 * `InstanceData.h:63-67` — `UPROPERTY( EditDefaultsOnly ) bool bCastDistanceFieldShadows = true;` — is
 * a per-mesh CONTENT setting. `true` is only the default, and CSS turning it off on a decorative mesh
 * is a deliberate cost decision rather than a bug. Ant's save measures 32-37% of components at `false`;
 * "repairing" a million authored-off instances would have overridden that decision wholesale, against a
 * GPU already reading 98%.
 *
 * ★ WHICH IS WHY THIS AUDIT SEPARATES FOUR POPULATIONS INSTEAD OF COUNTING ONE.
 * A single "N missing" number conflates content authoring with a defect and cannot be acted on either
 * way. Reading `AAbstractInstanceManager::InstanceMap` supplies the authored intent per component:
 *
 *   AUTHORED-OFF       authored false. Correct, expected, left alone. Also the LIVENESS PROOF for the
 *                      map read — a non-zero here is what shows the provenance lookup worked at all.
 *   LAZY-POISONED      authored true, but AbstractInstance stored false. The bug above, directly
 *                      observed. Expected to be 0 while the cvar is 0; non-zero means it got turned on.
 *   SHOULD-CONTRIBUTE  authored true, stored true, renderer flag still false. A genuine defect, and the
 *                      only bucket this will ever repair. **STRUCTURALLY ZERO ON THIS BUILD — see below.**
 *   FOREIGN            not owned by any `AAbstractInstanceManager`. Another mod's or the engine's
 *                      instanced meshes. Provenance unknown, so NOT judged and NOT repaired.
 *
 * ★ AND THE DEFECT BUCKET CANNOT FIRE AS THE CODE STANDS, WHICH IS A PROOF RATHER THAN A DISAPPOINTMENT.
 *
 * Every writer of `bAffectDistanceFieldLighting` reachable on this build, enumerated 2026-08-10:
 *
 *     AbstractInstanceManager.cpp:305                 = bCastDistanceFieldShadows  (at creation)
 *     AbstractInstanceManager.cpp:493                 = true                       (the dead lazy pass)
 *     FGBuildable.cpp:2314, :2350                     = false   USplineMeshComponent
 *     FGBuildable.cpp:2372, :2391, :2410              = false   UStaticMeshComponent
 *     FGProductionIndicatorInstanceComponent.cpp:14   = false   UFGColoredInstanceMeshProxy
 *                                                               (: UStaticMeshComponent)
 *
 * The bottom six are on types that are **not** `UInstancedStaticMeshComponent`, so this audit — which
 * gathers `GetComponents<UInstancedStaticMeshComponent>` — cannot see them. On a component it CAN see,
 * only the top two apply, and both set `true` for an authored-true mesh. Nothing clears it afterwards.
 *
 * ⚠ SO A ZERO HERE IS EXPECTED AND MEANS NOTHING ABOUT ANT'S WORLD. The report says exactly that rather
 * than printing "the theory is dead", which would read as evidence gathered in her save when it is a
 * fact about the source. **The distance-field explanation for rain-through-walls, light-through-terrain
 * and DF shadow pop-in is dead BY CONSTRUCTION; no boot was ever going to settle it.**
 *
 * The bucket stays because it is what would notice a THIRD writer appearing in a future game patch.
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
 * ⚠ AND UNTIL 2026-08-10 THAT REPAIR WAS UNSOUND, NOT MERELY EXPENSIVE. It walked every component with
 * the flag clear and set it — which on Ant's save is about a million instances that CSS authored off on
 * purpose. Turning the cvar on would not have "fixed" anything; it would have silently overridden the
 * game's own content decisions. It is now restricted to SHOULD-CONTRIBUTE, and it refuses to run at all
 * when provenance could not be established, because repairing what you cannot classify is guessing.
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

	/**
	 * Removes the sample ticker.
	 *
	 * ⚠ WITHOUT THIS, `FPMFixes::DisarmAll()` REPORTS THIS FIX DISARMED WHILE ITS TICKER KEEPS FIRING —
	 * into a module that is being torn down. Harmless at process exit, which is the only place
	 * DisarmAll has ever been called from, and exactly why nobody noticed; fatal to P4.2's master OFF
	 * switch, which needs Disarm to mean something mid-session.
	 */
	virtual void Disarm() override;

	/** `FPM.DistanceField.Audit` — count now and report. */
	static void AuditNow();

	/**
	 * ★ THE ONLY PLACE THAT TOUCHES `AAbstractInstanceManager`'s PROTECTED `InstanceMap`, AND IT HAS TO
	 * BE A MEMBER OF THIS CLASS.
	 *
	 * `AccessTransformers.ini` friends a CLASS. A free function in an anonymous namespace is a member of
	 * nothing and inherits no access — `FFPMPowerWarningProbe` learned this the same way, and the
	 * compiler names the field rather than the reason (`error C2248: cannot access protected member`).
	 *
	 * GAME THREAD ONLY. It walks actors, so it cannot run beside the parallel analyse phase; it is
	 * called during GATHER and the resulting map is then read-only for the rest of the audit.
	 *
	 * ⚠ NO COMPONENT POINTER IS DEREFERENCED HERE — they go in as keys and nothing else. The map is a
	 * lookup table, so a stale key can only ever fail to match, never crash.
	 *
	 * @param OutProvenance  filled with one entry per component AbstractInstance owns. Components absent
	 *                       from it are `Foreign` by definition, so nothing writes that value.
	 * @return how many `AAbstractInstanceManager` actors were found. **Zero means provenance is UNKNOWN
	 *         for the whole world**, which is not the same as "everything is foreign" — the report and
	 *         the repair both have to treat it as not-measured, or an absent manager reads as a clean
	 *         bill of health.
	 */
	static int32 BuildProvenance(UWorld* World,
	                             TMap<const UInstancedStaticMeshComponent*, EFPMDfProvenance>& OutProvenance);
};
