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

## 2026-08-08 18:30 — CODE — false comments removed, and one throttle policy instead of five literals

No version bump: behaviour is unchanged, the divisors keep the same values, and no log string moved.
This entry exists because the corrections are claims, and a claim is worth the same record as a fix.

- **What:** four comment corrections and one refactor.
  1. `FPMHologramNetGuard.cpp` — the role gate's comment claimed it "rejects essentially everything".
     It does not. This fix is client-only, and on a CLIENT most replicated actors ARE
     `ROLE_SimulatedProxy`, so the compare passes often and the `IsA` cast behind it does the real
     narrowing. Ordering still correct, stated reason wrong.
  2. `FPMHologramNetGuard.h` — `NeverOnDedicatedServer` was justified as "a build preview is renderer
     work". The fix contract explicitly rejects that shape of argument, and holograms are gameplay
     objects the server has authority over. The real reason is a cost one: the hook target is
     `AActor::DispatchBeginPlay`, which runs for every actor, and a dedicated server never holds a
     hologram as a simulated proxy — so there it could fire constantly and never once reach its body.
  3. `FPMHologramNetGuard.cpp` — the counter comment said "nothing here a second world load should
     reset". The totals are right, but the `N == 1` arm fires once per PROCESS, so load two prints no
     first-sighting line. Counts true, readout quieter than claimed.
  4. `CHANGELOG.md` — the two `(Corrected 2026-08-08: ...)` notes below, on the 475 figure and the
     hook macro.
- **The refactor:** five throttle sites used `50` / `100` / `200` as bare literals with no stated rule.
  They now share `FPMLog::ThrottleRoutine` / `ThrottleNotable` in `Core/FPMFixContract.h`, with the
  policy written once: the divisor encodes how EXPECTED an event is. An event believed unreachable is
  logged UNTHROTTLED and deliberately has no constant — a `% 1` divisor would be dead code dressed as
  policy.
- **⚠ AND THE REFACTOR TAUGHT SOMETHING WORTH KEEPING: THIS MODULE IS A UNITY BUILD.** Putting the
  constants in each `.cpp`'s anonymous namespace produced `error C2374: redefinition` — UE concatenates
  the `.cpp` files into one translation unit, so two anonymous namespaces declaring the same name are
  one namespace declaring it twice. **File-local constants are not file-local here.** Anything shared
  belongs in a header; anything genuinely per-fix needs a name unique across the whole module. Recorded
  in the header so the next person meets it as a rule rather than as a compile error.
- **Files:** `Public/Core/FPMFixContract.h`, `Private/Fixes/Interop/FPMHologramNetGuard.cpp`,
  `Public/Fixes/Interop/FPMHologramNetGuard.h`,
  `Private/Fixes/Interop/FPMInventoryInitGuard.cpp`, `CHANGELOG.md`.
- **Revert:** the comments are inert; the throttle constants can go back to literals without behaviour
  change.
- **Verified:** compiles clean, and the negative case too — `grep` finds no `GThrottle` symbol left in
  either fix. **NOT boot-tested.**

## 2026-08-08 18:09 — CODE — inventory init: stop writing the save, and stop destroying items

- **What:** every `Resize()` is now gated behind `HasAuthority()`, both `Scope.Override(0)` calls are
  deleted, an inventory the asset authored at zero is left alone in hook 2 as well as hook 1, and two
  counters were added for the branches that deliberately do nothing.
- **Why — ZERO RESIDUE, and vanilla's own header is the receipt.** `FGInventoryComponent.h:624-626`:
  *"When we resize the inventory we save how much bigger or smaller the inventory was made"*,
  `UPROPERTY(SaveGame) int32 mAdjustedSizeDiff`. `mInventoryStacks` is `SaveGame` too (`:652`), and
  `Resize`'s body is a link stub (`FGInventoryComponent.cpp:54`), so how much it persists is not
  readable. Repairing on the authority wrote residue into Ant's save. The law is prevention, not
  cleanup — an uninstalled mod cannot unwrite a saved field.
- **⚠ AND THE GATE COSTS NO COVERAGE, BECAUSE `Side::Any` WAS AN OVER-CORRECTION MADE THE SAME MORNING.**
  The bug is client-only by this fix's own evidence — *"crash on JOIN, singleplayer fine"*, because on a
  joining client components replicate before their owners finish initialising while on the host the
  owner initialised long ago. Widening it to the authority was applied on the general rule "all the
  fixes should run on the server too"; that rule is for fixes whose EFFECT reaches clients through
  replicated state, not for a client-local race. It bought nothing and paid in save residue. The hook
  still ARMS everywhere so the server can REPORT a zero-slot inventory — what is gated is the mutation,
  not the observation.
- **No item can be destroyed on any path.** `Override(0)` reads as safe and is not: the ordinary caller
  shape is remove-from-source-then-add-to-destination, so returning 0 after the removal means the items
  exist nowhere. Ant: *"we cant delete items. illegal."* Every branch that declines to repair now falls
  through to vanilla unchanged — the outcome an unmodded game produces, loud and recoverable rather
  than silent and permanent.
- **Two contradictions removed.** Hook 2 used to invent a slot on an inventory authored at zero, four
  lines from hook 1's stated principle that the asset already decided that. And its success log claimed
  "no items lost" while running BEFORE vanilla's AddStack, so the claim was unknowable at that point.
- **Header corrected.** It claimed AddStack "cannot reach the assert" after hook 1, then described two
  ways it can. It now states the narrow truth and names all three remaining populations, and says
  plainly that this is a repair at the earliest reachable choke point rather than a fix at the origin —
  so Ant can judge whether that satisfies "fixed at the source".
- **Files:** `Private/Fixes/Interop/FPMInventoryInitGuard.cpp`,
  `Public/Fixes/Interop/FPMInventoryInitGuard.h`.
- **Revert:** drop `FPMFixes::Arm(FFPMInventoryInitGuard::Get())` from `StartupModule`.
- **Verified:** compiles clean. Positive — `ON THE AUTHORITY`, `authored with ZERO slots`,
  `authority-seen`, `Gave it its authored` present UTF-16 in the DLL. Negative — the two 0.2.0
  item-loss strings are gone, `grep` finds no `Override(` call in the file, and both `Resize()` calls
  sit behind a `HasAuthority()` early-return in both hooks. **NOT boot-tested.**

## 2026-08-08 18:09 — VERSION — 0.2.1 → 0.2.2

- **What:** `VersionName` / `SemVersion` → `0.2.2`.
- **Why:** a repair to an existing fix with no new capability is a PATCH under SemVer.
- **Files:** `FicsitsPerformanceManager.uplugin`, `CHANGELOG.md`.
- **Revert:** set both fields back. Never published.
- **Verified:** build-only. **NOT boot-tested.**

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
  decorators. So the old read would have returned empty on every fire and sent every fire (100-2,150 a
  session, median ~1,050) into the residual cancel — **shipping the exact regression the fix exists to remove**, while its header
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
  cancelled `DispatchBeginPlay` outright for all such holograms — **100 to 2,150 a session, median ~1,050**, mostly vanilla.
  *(Corrected 2026-08-08: this said "475 a session". That was one log's peak, third-lowest of eight measured — 100, 275, 475, 1000, 1100, 1150, 1525, 2150 — so it understated the typical case ~2.2x and the worst ~4.5x. The review that caught it said 475 was the smallest of the eight; it is not, 100 is. Both the original claim and its correction were wrong in the same direction: quoting one number for a distribution.)*
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
- **Hook targets:** `UFGInventoryComponent::BeginPlay` — virtual, `FGInventoryComponent.h:187`,
  hooked with **`SUBSCRIBE_METHOD_VIRTUAL_AFTER`**. `UFGInventoryComponent::AddStack` — virtual,
  `:271`, hooked with `SUBSCRIBE_METHOD_VIRTUAL`.
  *(Corrected 2026-08-08: originally "Both `SUBSCRIBE_METHOD_VIRTUAL`", which the very next sentence contradicted by explaining the after-handler rule.)* ⚠ An SML *after*
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
