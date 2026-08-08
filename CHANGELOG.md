# CHANGELOG — FICSIT's Performance Manager (FPM2)

Append-only, **newest entry at the top**. One entry per discrete change — do not batch, and do not defer
to release time, because that is how release notes end up guessed after the fact instead of recorded as
they happened.

**Entry format** (`sf-changelog`):

```
## <YYYY-MM-DD HH:MM> — <CATEGORY> — <one-line what>
- **What:**     the precise change
- **Why:**      the bug or regression that motivated it
- **Files:**    paths touched (+ version bump if any)
- **Revert:**   how to undo, and the condition under which to undo it
- **Verified:** boot-test id (SP / dedicated+client) · build-only · NOT-YET
```

`CATEGORY` ∈ `CODE` · `CONFIG` · `SPEC` · `MOD` · `VERSION` · `WORLD/TEST`.

**Versioning is [Semantic Versioning 2.0.0](https://semver.org).** In the `0.y.z` band that means:

| change | bump | example |
|---|---|---|
| new fix, new capability, new surface | **MINOR** | `0.2.0` → `0.3.0` |
| repair to an existing fix, no new capability | **PATCH** | `0.2.0` → `0.2.1` |
| first public release / the mod is relied on | **MAJOR** | → `1.0.0` |

Initial development starts at **`0.1.0`**, per SemVer's own guidance — not `0.0.1`. `SemVersion` and
`VersionName` in the `.uplugin` are always equal; `Version` is the first digit of `SemVersion`. The
runtime does **not** carry a baked version literal — `StartupModule` reads it from the loaded plugin
descriptor, so there is no second place to bump and no way for the log line to disagree with the file.

Entries since the last `VERSION` line are the draft release notes for the next ficsit.app upload.

---

## 2026-08-08 17:56 — CODE — hologram net repair read the WRONG OBJECT, twice over

- **What:** the repair now calls vanilla's own
  `AFGBuildable::CreateAttachmentPointsFromComponents` on the CDO of the replicated `mBuildClass`,
  instead of reading `UFGAttachmentPointComponent`s off the hologram actor. The residual skip is
  narrowed to **modded** classes only. A fourth counter and a log line were added for the
  vanilla-no-points path.
- **Why — found by the `vox-review` pass that 0.2.0 owed, and it was a BLOCKER.** `0.2.0` read
  `Hologram->GetComponents<UFGAttachmentPointComponent>()`. Vanilla does not put them there.
  `FGBuildable.cpp:2539-2566` (real code, not a link stub) reads the buildable's DECORATION TEMPLATE
  plus the buildable's own components, and uses the hologram only as `owner` to filter by usage.
  Settled independently from asset bytes: of **400** exported assets containing
  `FGAttachmentPointComponent`, **350** are `Deco_*` decoration templates and nearly all the rest are
  decorators. So the old read would have returned empty on every fire and sent all ~475 into the
  residual cancel — **shipping the exact regression the fix exists to remove**, while its header
  claimed "Expected: zero".
- **⚠ THE SAME MISTAKE AS THE RAIN FIX — right intent, right hook, WRONG OBJECT — made in a file whose
  own comment cited rain as the lesson.** Citing a lesson is not applying it.
- **Second defect, caught in self-review after the first fix:** the residual `Scope.Cancel()` fired
  whenever the rebuild produced nothing — which is the NORMAL outcome for most of the game, since only
  400 assets have the component at all. That would have skipped BeginPlay for nearly every vanilla
  ghost: the same regression, one layer further down. Now split by ORIGIN — a FactoryGame class with no
  points falls through (they ship that way and do not assert); a modded class with no points is the
  carousel shape and is still skipped rather than crashing a joining client.
- **Timing half:** reading the CDO also fixes it. A CDO's subobjects and `mDecoratorClass` exist before
  any BeginPlay; the hologram's own components are copied in by `SetupComponents` DURING BeginPlay
  (`FGHologram.h:633-636`), so the old code ran before its own inputs existed.
- **Files:** `Private/Fixes/Interop/FPMHologramNetGuard.cpp`,
  `Public/Fixes/Interop/FPMHologramNetGuard.h`.
- **Revert:** drop `FPMFixes::Arm(FFPMHologramNetGuard::Get())` from `StartupModule`.
- **Verified:** FactoryEditor Win64 Development compiles clean; `VANILLA class with no attachment
  points` and `vanilla-no-points` present as UTF-16 literals in the built DLL. **NOT boot-tested** —
  and the load-bearing hypothesis (that no vanilla hologram asserts on an empty cache) is settled by
  one boot reading the four counters.

## 2026-08-08 17:56 — VERSION — 0.2.0 → 0.2.1

- **What:** `VersionName` / `SemVersion` → `0.2.1`.
- **Why:** a repair to an existing fix with no new capability is a **PATCH** under SemVer. Bumped rather
  than amended in place because a `0.2.0` binary already exists on disk, and two builds carrying one
  version with different behaviour is the defect this changelog's own header warns about.
- **Files:** `FicsitsPerformanceManager.uplugin`, `CHANGELOG.md`.
- **Revert:** set both fields back. Never published, so no consumer to migrate.
- **Verified:** build-only. **NOT boot-tested.**

## 2026-08-08 08:07 — VERSION — 0.1.0 → 0.2.0

- **What:** `VersionName` and `SemVersion` → `0.2.0`. Also fills in the `.uplugin` `Description`, which
  shipped blank in the first build — that field is the human-readable fix ledger `sf-packfix` requires.
- **Why:** two new fixes are new functionality, and SemVer bumps **MINOR** for that in the `0.y.z` band,
  not PATCH. Bumping at all (rather than rebuilding `0.1.0` in place) is non-negotiable: two builds
  carrying the same version and different behaviour is exactly the defect the 2026-08-08 triage filed
  against the old mod, whose EGS and Steam DLLs disagreed at one `SemVersion`.
- **⚠ RENUMBERED.** This build and its predecessor were originally cut as `0.0.2` and `0.0.1`. Both were
  wrong under SemVer — initial development starts at `0.1.0`, and adding two fixes is a MINOR bump, not
  a patch. Renumbered to `0.1.0` / `0.2.0` on 2026-08-08. **Free to do: nothing has ever been published
  to ficsit.app, so no installed copy anywhere carries the old numbers.** Git history still shows the
  original strings in commit subjects `d14a882` and `cf071d1`; the `.uplugin` is authoritative.
- **Files:** `FicsitsPerformanceManager.uplugin`, `CHANGELOG.md`.
- **Revert:** set both fields back and re-cut. No consumer to migrate.
- **Verified:** build-only. **NOT boot-tested.**

## 2026-08-08 08:00 — CODE — hologram net repair: rebuild the attachment points instead of skipping BeginPlay

- **What:** `FPMHologramNetGuard` hooks `AActor::DispatchBeginPlay`. On a `ROLE_SimulatedProxy`
  `AFGBuildableHologram` with an empty `mCachedAttachmentPoints`, it rebuilds the cache from the actor's
  own `UFGAttachmentPointComponent`s and falls through, so BeginPlay runs normally. The old mod
  cancelled `DispatchBeginPlay` outright for all such holograms — 475 a session, mostly vanilla.
- **Why:** the crash is `Assertion failed: mCachedAttachmentPoints.Num() > 0`,
  `ModularStations\FGCarouselHologram.cpp:55`, arriving through `UActorChannel::ProcessBunchInternal` —
  it kills a JOINING client, which is the mechanism that rebinds a player to a fresh character and loses
  their inventory. But `DispatchBeginPlay` also dispatches BeginPlay to every COMPONENT, so cancelling
  it left the remote build preview unrendered. The 2026-08-08 triage filed that cost as a HYPOTHESIS
  because FactoryGame's BeginPlay bodies are stubbed in the SML tree. **Ant had seen it:** *"i actually
  HAVE seen this. sometimes the holograms never rendered when sunfry held them in front of me."* Her
  instruction: *"we dont cut ANY feature here, just fix the core issue and keep 100% features complete."*
- **Mechanism:** `mCachedAttachmentPoints` is VANILLA (`FGBuildableHologram.h:523`), a plain TArray with
  no `UPROPERTY`, derived during the local placement flow and never replicated. The proxy arrives
  holding the components and an empty cache summarising them, so the data was there all along — the same
  shape as the rain fix. Rebuilt via the public `FACTORYGAME_API`
  `UFGAttachmentPointComponent::CreateAttachmentPoint(AActor*)` (`FGAttachmentPointComponent.h:28`),
  skipping `EAPU_BuildableOnly` points. Safe because a simulated proxy never makes a placement decision
  — snapping is resolved on the owning machine and committed by the server.
- **Hook target:** `AActor::DispatchBeginPlay`, non-virtual, so plain `SUBSCRIBE_METHOD`. Kept as the
  target rather than `AFGHologram::BeginPlay` because `SUBSCRIBE_METHOD_VIRTUAL` patches only the class
  it is given, and `AFGCarouselHologram` overrides BeginPlay — the mod's own asserting body would run
  unpatched.
- **Residual:** if the rebuild yields no points, BeginPlay is still skipped, because a join crash costs a
  session and a missing preview does not. It logs the class name every time, unthrottled, since the
  expected count is zero. If a vanilla class ever appears there, the skip must narrow to modded classes.
- **Files:** `Public/Fixes/Interop/FPMHologramNetGuard.h`,
  `Private/Fixes/Interop/FPMHologramNetGuard.cpp`, `Config/AccessTransformers.ini` (new friend:
  `AFGBuildableHologram` → `FFPMHologramNetGuard`, a **write**, unlike the read-only clone-sensor entry),
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert:** drop `FPMFixes::Arm(FFPMHologramNetGuard::Get())` from `StartupModule`. The
  AccessTransformer friend entry is then unused but harmless.
- **Verified:** FactoryEditor Win64 Development compiles clean; `hologram-net` and
  `rebuilt %d attachment point` present as UTF-16 literals in the built DLL. **NOT boot-tested.**

## 2026-08-08 08:00 — CODE — inventory init repair: size the inventory instead of refusing the items

- **What:** `FPMInventoryInitGuard` installs two hooks. `UFGInventoryComponent::BeginPlay` (AFTER) sizes
  an inventory that finished BeginPlay with zero slots to its authored `mDefaultInventorySize`.
  `UFGInventoryComponent::AddStack` stays as a backstop, but repairs in place and falls through to
  vanilla rather than refusing.
- **Why:** the crash is `Assertion failed: mInventoryStacks.Num() > 0`, `FGInventoryComponent.cpp:591`,
  reached from the Blueprint VM through `FLatentActionManager::TickLatentActionForObject` — a latent
  action resuming on a later tick against a component that replicated before its owner finished
  initialising. The old fix answered `Scope.Override(0)`, which destroys items whenever the caller had
  already removed them from the source, and it was gated on process type rather than authority, so **on
  a listen host it took that branch on the machine that owns the save.** Ant: *"we cant delete items.
  illegal. we need to not make zero inventory crates at all. it needs to be fixed at the source, not a
  bandaid solution. we still need a guard IF the source fix ever fails, but still"*
- **Hook targets:** `UFGInventoryComponent::BeginPlay` — virtual, `FGInventoryComponent.h:187`.
  `UFGInventoryComponent::AddStack` — virtual, `:271`. Both `SUBSCRIBE_METHOD_VIRTUAL`. ⚠ An SML *after*
  handler takes **no `Scope`**: `AddHandlerAfter` wants `TFunction<void(C*)>` exactly
  (`NativeHookManager.h:525`), because vanilla has already run and there is nothing left to cancel.
  Writing `auto& Scope` is a compile error.
- **Side change:** client-only → **`Any`**. A zero-slot inventory on the server is just as wrong, and
  asserting there kills every player's session rather than one. Per Ant: *"all the fixes should run on
  the server too."*
- **Remaining refusal path:** only if `AddStack` is reached OFF the game thread, where `Resize` is not
  safe. It logs at Error and says plainly that items may be lost. Believed unreachable; if it ever
  prints, the fix needs a game-thread hop, not a wider guard.
- **Files:** `Public/Fixes/Interop/FPMInventoryInitGuard.h`,
  `Private/Fixes/Interop/FPMInventoryInitGuard.cpp`, `Config/AccessTransformers.ini` (new friend:
  `UFGInventoryComponent` → `FFPMInventoryInitGuard`, read-only),
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert:** drop `FPMFixes::Arm(FFPMInventoryInitGuard::Get())` from `StartupModule`.
- **Verified:** FactoryEditor Win64 Development compiles clean; `inventory-init` present as a UTF-16
  literal in the built DLL. **NOT boot-tested.**

---

## 2026-08-08 07:30 — VERSION — 0.1.0 (first build of the rewrite)

- **What:** first cut of FPM2. Root Mod Modules (`RootInstance_` / `RootGameWorld_`), `Core/` (hook
  ledger, fix contract, box cache, dev overlay), and four fixes: static-base movement, no-owner RPC
  gate, clone sensor, rain occlusion repair.
- **Why:** a clean rewrite of the old mod under the Track A design, structured per the ficsit community
  docs so the shape stays recognisable to other modders.
- **Result:** **rain occlusion measured 35 errors/session → 0** on Ant's save. Controlled: the baseline
  log emitted exactly 35 `LogRainSystem` lines and all 35 were the error, so zero lines means zero
  errors and not a silenced system.
- **Files:** the initial tree.
- **Revert:** n/a — first build.
- **Verified:** booted on Ant's client 2026-08-08; 7 hooks armed, both root modules discovered,
  lifecycle in documented order. Shipped with an empty `.uplugin` `Description` — corrected in `0.2.0`.
