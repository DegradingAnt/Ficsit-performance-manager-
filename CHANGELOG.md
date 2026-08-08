# CHANGELOG — FICSIT's Performance Manager (FPM2)

Append-only, newest first. Category `CODE` for a native hook, `CONFIG` for settings, `VERSION` for a
bump. Entries since the last `VERSION` line are the draft release notes for the next ficsit.app upload.

---

## 0.0.2 — 2026-08-08

⚠ **A `vox-review` pass is OWED on this bump and has NOT been run.** Ant's standing rule: the bump is
the trigger and the last cheap moment, because past it a defect costs one of her boots. The version was
raised anyway rather than left alone, because leaving it would have produced two different `0.0.1`
builds with different behaviour — exactly the defect the 2026-08-08 triage flagged against the old
mod's EGS binary. **Run the review before packaging or booting.**

### VERSION — 0.0.1 → 0.0.2

Also fills in the `.uplugin` `Description`, which was empty. That field is the human-readable fix
ledger and it shipped blank in 0.0.1.

### CODE — hologram net repair: rebuild the attachment points instead of skipping BeginPlay

- **What.** `FPMHologramNetGuard` hooks `AActor::DispatchBeginPlay`. On a `ROLE_SimulatedProxy`
  `AFGBuildableHologram` with an empty `mCachedAttachmentPoints`, it rebuilds the cache from the
  actor's own `UFGAttachmentPointComponent`s and falls through, so BeginPlay runs normally. The old
  mod cancelled `DispatchBeginPlay` outright for all such holograms — 475 a session, mostly vanilla.
- **Why.** The crash is `Assertion failed: mCachedAttachmentPoints.Num() > 0`,
  `ModularStations\FGCarouselHologram.cpp:55`, arriving through `UActorChannel::ProcessBunchInternal`
  — it kills a JOINING client, which is the mechanism that rebinds a player to a fresh character and
  loses their inventory. But `DispatchBeginPlay` also dispatches BeginPlay to every COMPONENT, so
  cancelling it left the remote build preview unrendered. The 2026-08-08 triage filed that cost as a
  HYPOTHESIS because FactoryGame's BeginPlay bodies are stubbed in the SML tree. **Ant had seen it:**
  *"i actually HAVE seen this. sometimes the holograms never rendered when sunfry held them in front of
  me."* Her instruction: *"we dont cut ANY feature here, just fix the core issue and keep 100% features
  complete."*
- **Mechanism.** `mCachedAttachmentPoints` is VANILLA (`FGBuildableHologram.h:523`), a plain TArray
  with no `UPROPERTY`, derived during the local placement flow and never replicated. The proxy arrives
  holding the components and an empty cache summarising them, so the data was there all along —
  the same shape as the rain fix. Rebuilt via the public `FACTORYGAME_API`
  `UFGAttachmentPointComponent::CreateAttachmentPoint(AActor*)` (`FGAttachmentPointComponent.h:28`),
  skipping `EAPU_BuildableOnly` points. Safe because a simulated proxy never makes a placement
  decision — snapping is resolved on the owning machine and committed by the server.
- **Hook target.** `AActor::DispatchBeginPlay` — verified **non-virtual** at engine
  `GameFramework/Actor.h:2149` (`ENGINE_API void DispatchBeginPlay(bool)`), so plain `SUBSCRIBE_METHOD`
  is correct, not `_VIRTUAL`. Kept as the target rather than `AFGHologram::BeginPlay` because
  `SUBSCRIBE_METHOD_VIRTUAL` patches only the class it is given, and `AFGCarouselHologram` overrides
  BeginPlay — the mod's own asserting body would run unpatched.
- **Residual.** If the rebuild yields no points, BeginPlay is still skipped, because a join crash costs
  a session and a missing preview does not. It logs the class name every time, unthrottled, since the
  expected count is zero. If a vanilla class ever appears there, the skip must narrow to modded classes.
- **Files.** `Public/Fixes/Interop/FPMHologramNetGuard.h`,
  `Private/Fixes/Interop/FPMHologramNetGuard.cpp`, `Config/AccessTransformers.ini`,
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert.** Drop `FPMFixes::Arm(FFPMHologramNetGuard::Get())` from `StartupModule`. The
  AccessTransformer friend entry is then unused but harmless.
- **Verified.** FactoryEditor Win64 Development compiles clean; `hologram-net` and
  `rebuilt %d attachment point` present as UTF-16 literals in the built DLL. **NOT boot-tested.**

### CODE — inventory init repair: size the inventory instead of refusing the items

- **What.** `FPMInventoryInitGuard` installs two hooks. `UFGInventoryComponent::BeginPlay` (AFTER)
  sizes an inventory that finished BeginPlay with zero slots to its authored
  `mDefaultInventorySize`. `UFGInventoryComponent::AddStack` stays as a backstop, but repairs in place
  and falls through to vanilla rather than refusing.
- **Why.** The crash is `Assertion failed: mInventoryStacks.Num() > 0`,
  `FGInventoryComponent.cpp:591`, reached from the Blueprint VM through
  `FLatentActionManager::TickLatentActionForObject` — a latent action resuming on a later tick against
  a component that replicated before its owner finished initialising. The old fix answered
  `Scope.Override(0)`, which destroys items whenever the caller had already removed them from the
  source, and it was gated on process type rather than authority, so **on a listen host it took that
  branch on the machine that owns the save.** Ant: *"we cant delete items. illegal. we need to not make
  zero inventory crates at all. it needs to be fixed at the source, not a bandaid solution. we still
  need a guard IF the source fix ever fails, but still"*
- **Hook targets.** `UFGInventoryComponent::BeginPlay` — virtual, `FGInventoryComponent.h:187`.
  `UFGInventoryComponent::AddStack` — virtual, `:271`. Both `SUBSCRIBE_METHOD_VIRTUAL`. ⚠ An SML
  *after* handler takes **no `Scope`**: `AddHandlerAfter` wants `TFunction<void(C*)>` exactly
  (`NativeHookManager.h:525`), because vanilla has already run and there is nothing left to cancel.
  Writing `auto& Scope` is a compile error.
- **Side change.** Client-only → **`Any`**. A zero-slot inventory on the server is just as wrong, and
  asserting there kills every player's session rather than one. Per Ant: *"all the fixes should run on
  the server too."*
- **Remaining refusal path.** Only if `AddStack` is reached OFF the game thread, where `Resize` is not
  safe. It logs at Error and says plainly that items may be lost. Believed unreachable; if it ever
  prints, the fix needs a game-thread hop, not a wider guard.
- **Files.** `Public/Fixes/Interop/FPMInventoryInitGuard.h`,
  `Private/Fixes/Interop/FPMInventoryInitGuard.cpp`, `Config/AccessTransformers.ini`,
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert.** Drop `FPMFixes::Arm(FFPMInventoryInitGuard::Get())` from `StartupModule`.
- **Verified.** FactoryEditor Win64 Development compiles clean; `inventory-init` present as a UTF-16
  literal in the built DLL. **NOT boot-tested.**

---

## 0.0.1 — 2026-08-08

First build of the rewrite. Root Mod Modules, `Core/` (hook ledger, fix contract, box cache, dev
overlay), and five fixes: static-base movement, no-owner RPC gate, clone sensor, rain occlusion repair,
plus the overlay itself. **Rain occlusion measured 35 errors/session → 0** on Ant's save, controlled
against a baseline log in which all 35 `LogRainSystem` lines were the error.

Shipped with an empty `.uplugin` `Description` — corrected in 0.0.2.
