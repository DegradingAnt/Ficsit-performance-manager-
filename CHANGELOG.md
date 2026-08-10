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

**⚠ THE TEST FOR MINOR IS "WOULD A PLAYER NOTICE", NOT "IS IT NEW".** Got this wrong TWICE on
2026-08-09 — first cutting a `0.6.0` for two overlay cvars, then again for a set of debug console
commands. Ant, both times: *"why the big bump for a small patch"* and *"the big bump is kinda too
much for a small edit."*

> **Ask: would Ant notice this while just PLAYING, with the console closed?**
> **No → PATCH.** Whatever it is, however new, however much code it took.
> **Yes → MINOR.**

A first attempt at this rule asked *"could you describe the release without mentioning the new
command?"* — and that is **not good enough**, because it passes any sufficiently novel thing. Four
brand-new console commands are genuinely new and still invisible to someone playing the game.

By the player-notice test:

| change | bump | why |
|---|---|---|
| a fix that stops a crash or a stutter | MINOR | she feels it |
| a governor that moves her settings | MINOR | she sees it |
| debug console commands, diagnostics, probes | **PATCH** | invisible unless you open the console |
| repairs to an existing surface | **PATCH** | it already existed, it just works now |
| a cvar added so a repair is configurable | **PATCH** | plumbing for the repair |

**Why the pacing matters at 0.x:** this mod ships several builds a day and has not yet shipped a
governor. Spending a MINOR on internal instruments reaches `1.0` in a fortnight while the thing the
mod exists to do is still unbuilt. The version number should track what the mod DOES for her, not how
much work went in.

Initial development starts at **`0.1.0`**, per SemVer's own guidance — not `0.0.1`. `SemVersion` and
`VersionName` in the `.uplugin` are always equal; `Version` is the first digit of `SemVersion`. The
runtime does **not** carry a baked version literal — `StartupModule` reads it from the loaded plugin
descriptor, so there is no second place to bump and no way for the log line to disagree with the file.

Entries since the last `VERSION` line are the draft release notes for the next ficsit.app upload.

---

## 2026-08-10 19:05 — CODE — three report commands that printed nothing, a seventh cause bucket, and a guard demoted by its own measurement

- **What:** the fixes the first driven boot found, plus the one idea worth taking from mining two mods.
- **Why:** Ant ran three FPM report commands in the console and got blank lines. All three had worked.

- **★ THREE COMMANDS ANSWERED WHERE NOBODY WAS LOOKING.** `FPM.Pso.Report`, `FPM.Stall.Report` and
  `FPM.Blueprint.Report` were `FAutoConsoleCommand` + `UE_LOG(Display)`. A Display-level log does not
  echo to the in-game console, so each ran, wrote a full answer to `FactoryGame.log`, and showed nothing.
  `sf-boottest` names this exact trap — *"a command that looks dead has usually run"* — and three shipped
  with it anyway. New `FPMScopedConsoleEcho` (RAII, attaches the console device to `GLog` for the call)
  fixes the category rather than the three instances, so a line added later is covered too.
  ⚠ Its destructor is load-bearing: the console's device dies with the call, and leaving it registered
  would leave `GLog` writing into freed memory from any thread.
  ⚠ And it is NEVER combined with a report that already takes an `FOutputDevice` — that would print
  every line twice. Two mechanisms, one per shape, stated at both call sites.

- **★ A SEVENTH HITCH CAUSE BUCKET: THE ASSET STREAMER.** The boot measured 9 world-load hitches, worst
  946.6 ms, mean 281.0 ms, **100% UNATTRIBUTED**, in a session logging 2973 sync loads. The meter could
  not ask whether the streamer was behind. It now samples `GetNumWantingResources()` per span and keeps
  the peak (`ContentStreaming.h:321`, overridden `:747` — "the number of resources that currently wants
  to be streamed in"), a LEVEL like the PSO run flag, with its own denominator.
  **Liveness:** `GetNumWantingResourcesID()` (`:332`) moves only when the streaming system updates, so a
  zero backlog is distinguishable from a readout nothing feeds. If it has never moved the summary says
  UNPROVEN instead of printing a confident 0.
  ⚠ The idea came from mining PreloadMap; none of its code did. That mod force-streams the map through a
  ghost viewer, which FPM does not adopt — Ant ran it and confirmed it lags everything.

- **★ THE NANITE GUARD DEMOTED TO A METER BY ITS OWN FIRST MEASUREMENT.** `1101 sample(s), 0 of them
  scaled down` in her real base — the quality-scale factor never left 1.00, so Nanite is not dropping
  geometric detail for pool pressure here and the raise had no trigger. Removed the raise, the VRAM
  sizing and the `[512, 2048]` clamp. `OriginStatus` downgraded `OriginNamed` → `UnknownCause`, because
  the mechanism it named was measured not to occur.
  ⚠ Its baseline was wrong anyway: it compared against the ENGINE default of 512 MB while this game runs
  **50** (`FactoryGame/Config/DefaultEngine.ini:47`, under `:45 DynamicallyGrowAllocations=1`).
  ⚠ And the report contradicted itself — `pool 50 MB` one line, "does not overcommit a 512 MB pool" the
  next, because the verdict interpolated the constant. It prints the live value now.

- **REVERSED ONE OF MY OWN FIX ITEMS: the glass re-assert STAYS.** `FPM.Glass.Report` printed "the
  re-assert is dead weight and can be deleted" — a verdict I wrote, fired on two samples. Deleting it
  would remove the only thing that can FALSIFY the mod's priority argument; if an update ever changes
  that ladder, the version with the check logs which cvar lost and the version without silently stops
  working. The hook only fires on a settings Apply, so it costs nothing between times.

- **Files:** `Public/Core/FPMConsoleEcho.h` + `Private/Core/FPMConsoleEcho.cpp` (new),
  `Public/Core/FPMHitchMeter.h` + `Private/Core/FPMHitchMeter.cpp`, `Private/Core/FPMStallSampler.cpp`,
  `Private/Fixes/Vanilla/FPMBlueprintSweepGate.cpp`,
  `Public/Fixes/ModFeatures/FPMNaniteStreamingGuard.h` + `.cpp`,
  `Private/Fixes/ModFeatures/FPMGlassQuality.cpp`, `FicsitsPerformanceManager.uplugin`
  (0.11.1 → **0.11.2**, PATCH: repairs and diagnostics, invisible with the console closed).
- **Revert:** `git revert`. The streaming bucket is additive; removing it returns the six-bucket line.
- **Verified:** build-only. `Result: Succeeded`; `check_structure.py` 26 fixes / 0 / 0. NOT-YET
  boot-tested — Ant is not booting Satisfactory tonight.

---

## 2026-08-10 17:35 — CODE — the measurement that decides the PSO pre-optimize exception

- **What:** the hitch meter now counts cold PSO creations that happen MID-PLAY — more than 30 s after a
  world load — separately from the startup burst, and `FPM.Pso.Report` prints a verdict naming the
  decision it feeds.
- **Why:** Ant, 2026-08-10, on whether FPM2 should spend an ini exception on
  `r.ShaderPipelineCache.PreOptimizeEnabled`: *"Defer until we have a startup measurement."* This is
  that measurement, and it exists so the ruling is made against a number instead of a rule.

- **Pre-optimize IS off in her game, confirmed from the retail cooked config rather than assumed.** The
  FModel dump of 1.2.3.1 / CL495413 shows `FactoryGame/Config/DefaultEngine.ini:15` setting
  `r.ShaderPipelineCache.Enabled=1` and nothing else, so `PreOptimizeEnabled` sits at the engine's own
  default of 0 (`ShaderPipelineCache.cpp:138-142`). There is something to turn on. Whether it is worth
  turning on is the open question.
- **Why `PsoCreatesTotal` could not answer it.** Pre-optimize front-loads compilation into startup, so
  it can only help pipelines that would otherwise be built LATER. The existing total is dominated by the
  arrival burst that pre-optimize would merely MOVE. The new counter isolates the part it would remove.
- **Not gated on a hitch, deliberately.** A 4 ms pipeline build costs real time and never trips the
  hitch threshold. Counting only the ones that hitched would undercount the prize and could report a
  confident zero on a session that spent seconds compiling.
- **The 30 s settle window is a judgement, and it errs long on purpose.** Erring long can only
  UNDERSTATE the case for pre-optimize, which is the safe direction when what is being decided is
  permanent residue on a player's machine. It resets on every world load, so a quit-to-menu-and-back
  does not let the next world's startup burst count as mid-play.
- **The verdict refuses to read as a pass when it is not one.** Four distinct outcomes: no world loaded
  yet (not a result), still inside the settle window (play longer), new-PSO reporting off (UNMEASURABLE
  — the zero means "not measured"), and only then zero-means-no or a count with the size of the prize.

- **⚠ Threading, caught before it shipped.** `OnPsoCreated` does not run on the game thread — every
  counter beside it is a `FThreadSafeCounter` for that reason — so the settle timestamp it compares
  against is `std::atomic<double>` with relaxed ordering, not a plain double. Relaxed is sufficient: it
  is a threshold against a clock, so reading a stale value misfiles at most one sample.

- **Files:** `Public/Core/FPMHitchMeter.h`, `Private/Core/FPMHitchMeter.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.11.0 → **0.11.1**, PATCH: a counter and a log line, invisible
  with the console closed).
- **Revert:** `git revert`. Nothing else depends on it.
- **Verified:** build-only. `Result: Succeeded`; `check_structure.py` 26 fixes / 0 / 0. NOT-YET
  boot-tested. The boot question is one command after a normal play session: `FPM.Pso.Report`. A zero
  mid-play count closes the ini exception for good; a large one reopens it with a number attached.

---

## 2026-08-10 17:05 — CODE — "the trains mesh went low poly": measured, fixed at runtime, and FPM1's ini exception was never needed

- **What:** a new fix, `FFPMNaniteStreamingGuard`. It reads Nanite's live quality-scale factor, and the
  first time that factor drops below 1.0 it raises `r.Nanite.Streaming.StreamingPoolSize` above the
  engine's 512 MB — then keeps measuring so the report can say whether the raise worked.
  `FPM.Nanite.PoolMB` is the lever, `FPM.Nanite.Report` is the measurement.
- **Why:** Ant, 2026-08-03: *"its a streaming issue of some sort. my gpu isnt maxed and the trains mesh
  went low poly as i loaded new terrain. something is still starved."* She asked today for research
  before an ini exception was spent on it, and the research changed the answer.

- **★ FPM1 REACHED FOR THE WRONG LEVER, AND PAID AN INI EXCEPTION FOR IT.** It wrote
  `r.Nanite.Streaming.MaxPageInstallsPerFrame` and `.MaxPendingPages` to `Engine.ini` because both are
  `ECVF_ReadOnly`. Those change how FAST pages arrive. The reported symptom is Nanite *deliberately
  dropping detail* because its pool is overcommitted, which is a different mechanism and reads a
  different number. `r.Nanite.Streaming.StreamingPoolSize` is `ECVF_RenderThreadSafe` with no
  `ECVF_ReadOnly`, so it is writable at runtime and FPM2 fixes this with an ordinary cvar hold and zero
  residue. **The exception was not needed and is not being carried.**

- **The engine says what is happening, in its own words** (`NaniteStreamingManager.cpp:129`):
  *"Controls for dynamically adjusting quality (pixels per edge) when the streaming pool is being
  overcommitted... can happen when rendering scenes with lots of unique geometry at high resolutions."*
  A megabase is that scene. The numbers: pool 512 MB (`:134`), scale down above 85% load (`:138`), floor
  0.3 (`:146`).

- **★ AND THE SCALER IS ASYMMETRIC, WHICH IS WHY A ONE-SECOND SPIKE LEAVES A LASTING ARTEFACT.**
  `FQualityScalingManager::Update` (`:1193-1226`): over budget for 2 frames running multiplies the scale
  by 0.97 — the engine's own comment is *"adjust quality down rapidly"* — while recovery needs 30
  consecutive good frames before it starts and then climbs 1% a frame. Falling to the 0.3 floor takes
  about forty frames; climbing back takes several seconds. Loading new terrain is exactly a short burst
  of page requests. That asymmetry is the gap between "a number moved" and "the train looked wrong".

- **It measures before it acts, and keeps measuring after.**
  `Nanite::FStreamingManager::GetQualityScaleFactor()` (`NaniteStreamingManager.h:88-91`) is a public
  header-inline accessor on `Nanite::GStreamingManager` (`:361`) — no hook, no access transformer, and
  no symbol that can be missing. A machine whose pool never overcommits **never gets a write at all**.
  When one does, the report compares the minimum before the raise against the minimum after it, and says
  NOT ENOUGH rather than implying success from the fact that a write happened.
  Liveness: 0 samples prints as a dead readout; "min 1.0 over N samples" prints as a real negative
  result, explicitly not as the fix working.
  ⚠ The write at `:3159` is on the ungated main path and `QualityScalingManager` is constructed
  unconditionally at `:1247`, so the meter is live in a shipping build — checked, because an instrument
  reading a value nothing writes is this project's most expensive recurring bug.

- **⚠ IT SHARES VRAM WITH THE TEXTURE POOL GUARD, AND THAT COUPLING IS NOW EXPLICIT.**
  `FPMTexturePoolGuard.cpp:26` reserved a literal `NaniteFloorMB = 512` — the same number as this pool's
  engine default — before sizing the texture pool. Raising one without the other would let textures
  claim VRAM Nanite is already using. `ComputePoolMB` now takes the reservation as a parameter and the
  caller passes `FFPMNaniteStreamingGuard::ReservedMB()`, which reads the live cvar. One declaration
  site, still clamped to the old floor. The texture pool guard's own comment turns out to have named
  this mechanism already: *"It fails globally at 85% occupancy"* — that 85 is `QualityScale.MaxPoolPercentage`.

- **The sizing is reasoned, not measured, and says so.** One eighth of the card clamped to [512, 2048]:
  the floor means this can never be worse than vanilla, and the cap keeps clear of the 2-4 GB buffer
  limit the engine warns about at `:141-143`. If the factor still drops after the raise, the cap is too
  low for her base and the report tells her to raise `FPM.Nanite.PoolMB` by hand.

- **Files:** `Public/Fixes/ModFeatures/FPMNaniteStreamingGuard.h` (new),
  `Private/Fixes/ModFeatures/FPMNaniteStreamingGuard.cpp` (new),
  `Public/Fixes/Interop/FPMTexturePoolGuard.h` + `Private/.../FPMTexturePoolGuard.cpp` (reservation is
  now an input), `Public/Core/FPMDiag.h` + `Private/Core/FPMDiag.cpp` (new `NaniteStreaming` channel),
  `Private/FicsitsPerformanceManager.cpp`, `FicsitsPerformanceManager.uplugin`
  (0.10.1 → **0.11.0**, MINOR: geometry that stops dropping to low detail is something she sees).
- **Revert:** `FPM.Nanite.PoolMB -1` leaves it as a pure meter with no write. Revert the commit to
  remove it — and if so, restore `NaniteFloorMB` as a constant in the texture pool guard.
- **Verified:** build-only. `Result: Succeeded`; `check_structure.py` 26 fixes / 0 / 0. NOT-YET
  boot-tested. The boot question: play until terrain streams in, then `FPM.Nanite.Report`. Either it
  never caught a drop (a real negative — her save does not reproduce it), or it caught one and the
  report says whether the bigger pool stopped it.

---

## 2026-08-10 16:20 — CODE — the distance-field audit across every core, and a readout that can say it did not

- **What:** `CountAndMaybeRepair` is now three phases — GATHER (serial), ANALYSE
  (`ParallelForWithTaskContext`, every core), REPAIR (serial). All three are timed and the timings are
  printed with every result.
- **Why:** Ant, 2026-08-10: *"do the parallise stuff for everything that CAN be done like that."* This
  audit walks every instanced mesh component in the world — 10,227 of them on her save — and did the
  whole walk on the game thread.

- **What CAN be parallel, and what provably cannot.** `TActorIterator` is a stateful cursor over the
  level's actor arrays with no parallel form, so GATHER cannot move. `MarkRenderStateDirty()` is not
  thread-safe, so REPAIR cannot move — doing it inside the loop would be a data race on the renderer,
  which is a worse bug than the one this audit exists to find. Only the read-only middle moves.
- **Why the UObject reads in the middle are legal.** The game thread BLOCKS inside `ParallelFor`, so GC
  cannot run and free a component under a worker. The same reads on a background thread would be
  unsafe. `GetInstanceCount()` allocates nothing, issues no render command and creates no UObject,
  which is what keeps it inside the safe set.
- **Offender names are resolved AFTER the loop.** Phase 2 carries component pointers only.
  `GetStaticMesh()->GetName()` builds an FString per offender, and at most five are ever printed, so
  the cost is paid once on the game thread for the five that survive the sort.

- **★ THE REPORT CAN FALSIFY THE CLAIM, which is the point.** It prints how many worker contexts
  `ParallelForWithTaskContext` actually created. **One means the engine ran the loop inline on the
  calling thread and nothing was parallel that run** — legal engine behaviour for a small `Num`
  (`ParallelFor.h:738-748`, `OutContexts.AddDefaulted(GetNumberOfThreadTasks(Num, 1, Flags))`), and
  invisible without this number. It also says so out loud when GATHER cost more than ANALYSE, because
  then the parallel phase is not where the time goes and parallelising further would buy little.
  "We parallelised it" is a claim, and this mod does not ship those unmeasured.

- **A bug caught while finishing the stashed draft.** The repair phase walks every component and
  re-tests the flag, and a comment claimed it walked the offender list instead. The offender list is
  capped at 64 PER WORKER CONTEXT — a sample for the log lines, never the complete set. Repairing from
  it would have fixed the first 64-per-worker and left the rest broken while reporting the world
  repaired. The code was right, the comment was wrong, and the comment is what would have been believed.

- **`MissingInstances` is now `int64`** — the worker contexts accumulate into `int64` and the struct
  took `int32`, so the two disagreed. Her save measures 87,965 instances, well inside `int32`, so this
  is a correctness tidy rather than a live overflow.

- **Files:** `Private/Fixes/Interop/FPMDistanceFieldAudit.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.10.0 → **0.10.1**, PATCH: an internal audit's cost profile,
  invisible with the console closed).
- **Revert:** `git revert`. Undo if the parallel phase ever reports a component count that disagrees
  with the serial version — that would mean the per-context accumulation is losing work.
- **Verified:** build-only. `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`; `check_structure.py` 25 fixes / 0 / 0. NOT-YET boot-tested. The boot question:
  does the audit still report 3273/10227 and 87965, and how many worker contexts did it get?

---

## 2026-08-10 15:55 — CODE — better glass, carried from FPM1 with the ini half removed and the poll removed

- **What:** a new fix, `FFPMGlassQuality`, in a new `Fixes/ModFeatures/` folder. It holds
  `r.Lumen.TranslucencyReflections.FrontLayer.Allow=1` **and** `.Enable=1` through `FPMCVarWriter`, so
  glass and windows reflect properly at every reflection-quality level. `FPM.Glass.Enable` is the
  toggle and defaults to ON. `FPM.Glass.Report` prints the state.
- **Why:** Ant, 2026-08-02, *"glass looks bad always tho"*, and 2026-08-10, *"the glass should live and
  die with the mod. make it so the mod turns it on and keeps it on... for now the main mod will just
  have a 'better glass' toggle."* The feature existed in FPM1 and did not survive the rewrite.

- **TWO cvars, not one, and that was the original bug.** The engine gate, read this session at
  `LumenFrontLayerTranslucency.cpp:55-58`, is
  `(PPV.LumenFrontLayerTranslucencyReflections || GLumen...Enabled) && GLumen...Allowed != 0 && ...`.
  `BaseScalability.ini:393` sets `FrontLayer.Allow=0` under `[ReflectionQuality@2]` — High, the common
  setting — so `.Enable=1` alone is ANDed out and does nothing. Both keys, or neither.

- **NO INI, AND THAT COSTS SOMETHING.** FPM1 shipped this as an `Engine.ini [SystemSettings]` line plus
  a runtime write, because an ini is read before mods exist. FPM2 is zero-residue, so the ini half is
  gone, and the loading screen plus the first frames after it can render with vanilla glass. Stated in
  the header rather than glossed. If that turns out to be visible in practice the answer is an earlier
  arm point, not an ini.

- **NO 2-SECOND RE-ASSERT LOOP, which is Ant's own question answered.** She asked *"but if it loops
  every 2 s then wont it lag the main thread?"* It would, so it does not loop. Both cvars are
  `ECVF_Scalability` (`LumenFrontLayerTranslucency.cpp:23`, `:40`), but the console manager refuses a
  write below the priority in force — `FConsoleVariableBase::CanChange` is `NewPri >= OldPri`
  (`ConsoleManager.cpp:267-272`). `FPMCVarWriter` holds at `ECVF_SetByPluginHighPriority` (0x07) and
  scalability writes at `ECVF_SetByScalability` (0x01), so the hold wins by construction.

- **⚠ EXPECT TWO `LogConsoleManager: Warning` LINES PER SETTINGS APPLY, AND THEY ARE OURS.** The engine
  logs every refused write. Naming them here and in the arm line, because an unexplained warning in her
  log is a cost the mod is imposing.

- **★ THE ARGUMENT ABOVE IS AN ARGUMENT, SO IT IS ALSO MEASURED.** `FFPMTexturePoolGuard` holds a cvar
  through the same writer and still logs "REPAIRED a scalability clobber", which means the priority
  reasoning is not settled by reading. So the fix hooks
  `UFGGameUserSettings::ApplyNonResolutionSettings` (`FGGameUserSettings.h:112`) with an `_AFTER`
  handler and reads both values back. Event-driven, so it costs nothing between settings changes. The
  counters settle it in one boot: 0 verifications prints as an UNRUN test rather than a clean one;
  verifications with 0 repairs confirms the priority argument and licenses deleting the re-assert; any
  repair proves the argument wrong and names which cvar lost.

- **⚠ ARM ORDER IS LOAD-BEARING.** This is the first fix that holds a cvar at Arm, and
  `FPMSaveSettingsInterceptor::Arm` refuses to arm when any hold already exists
  (`FPMSaveSettingsInterceptor.cpp:127-136` reads `GetHeldCVars` and calls `Fail()`), and `Fail()`
  latches for the session. Arming glass first would disable clause 6 for the whole boot. It is armed
  immediately after the interceptor, and the reason is written at the call site.

- **What is still left behind from FPM1, and why it is NOT in this commit.** FPM1's README named six
  keys it wrote to `Engine.ini [SystemSettings]`. Two are the glass pair, now carried. The other four,
  with their flags read from engine source this session:
  | key | flags | runtime-writable |
  |---|---|---|
  | `r.Nanite.Streaming.MaxPageInstallsPerFrame` | `ECVF_RenderThreadSafe \| ECVF_ReadOnly` (`NaniteStreamingManager.cpp:104-107`) | **no** |
  | `r.Nanite.Streaming.MaxPendingPages` | `ECVF_RenderThreadSafe \| ECVF_ReadOnly` (`:88-91`) | **no** |
  | `r.ShaderPipelineCache.PreOptimizeEnabled` | `ECVF_ReadOnly \| ECVF_RenderThreadSafe` (`ShaderPipelineCache.cpp:139-142`) | **no** |
  | `r.ShaderPipelineCache.PrecompileBatchTime` | `ECVF_Default \| ECVF_RenderThreadSafe` (`:91-94`) | yes |
  Three of the four are `ECVF_ReadOnly`, so there is no runtime path and carrying them needs an ini
  write, which zero-residue forbids. That is Ant's ruling to make, not a decision to take quietly —
  the Nanite pair was ONE of FPM1's own recorded governance failures, shipped on the strength of an
  existing exception's precedent. Raised as a question rather than built.

- **Files:** `Public/Fixes/ModFeatures/FPMGlassQuality.h` (new),
  `Private/Fixes/ModFeatures/FPMGlassQuality.cpp` (new), `Public/Core/FPMDiag.h`,
  `Private/Core/FPMDiag.cpp` (new `GlassQuality` channel),
  `Private/FicsitsPerformanceManager.cpp` (include + arm, after the interceptor),
  `FicsitsPerformanceManager.uplugin` (0.9.2 → **0.10.0**, MINOR: she sees this with the console
  closed, which is the player-notice test).
- **Revert:** `FPM.Glass.Enable 0` disables it at runtime with no restart. To remove it, revert the
  commit. Undo if front-layer reflections measure too expensive — and if so gate on `.Allow`, never on
  `.Enable` alone, which is the same bug in the other direction.
- **Verified:** build-only. `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`; `check_structure.py` 25 fixes / 0 errors / 0 warnings. NOT-YET boot-tested.
  The boot question is one line: run `FPM.Glass.Report`, press Apply in the settings menu, run it again.

---

## 2026-08-10 15:10 — CODE — review fixes: a disarm that disarms, two inert friends, three honest readouts

- **What:** the seven findings from the full-mod `vox-review` that were still open. No new capability.
- **Why:** every one is the same shape — the code was close to right and a comment claimed more than the
  code did. In a codebase where the comments ARE the design record, a false comment costs the next
  reader an hour and can cost a wrong fix.

- **1. `FFPMBlueprintSweepGate::Disarm` did not disarm.** It cleared the delegate handle and left the
  handler installed, under a comment reading *"the ledger owns the removal"*. `FPMHookLedger` exposes
  `Install` / `Records` / `LogInventory` and nothing else, and its own header states it does not own
  lifetimes. So the gate kept CANCELLING SWEEPS after Disarm reported it stopped. It now calls
  `UNSUBSCRIBE_METHOD`, guarded on `SweepHookHandle.IsValid()` — in the editor the ledger refuses the
  install and hands back an invalid handle, and `RemoveHandler` would then find both handler arrays
  empty and uninstall a hook that was never installed (`NativeHookManager.h:358-375`). This is the only
  fix in the mod that removes its hook, because it is the only one that can do harm by continuing.
  `FPM.Hooks.Dump` lists what INSTALLED, not what is live, and the comment now says so.

- **2. ⚠ TWO ACCESS TRANSFORMER ENTRIES WERE INERT, AND THE REASON GENERALISES.** Removed
  `Friend=(Class="AGameMode", FriendClass="FFPMCloneSensor")` and
  `Friend=(Class="ARecastNavMesh", FriendClass="FFPMNavMeshCeiling")`. Both members are PUBLIC:
  `InactivePlayerArray` sits at `GameMode.h:138` inside the `public:` at `:106`, and the `protected:`
  the old comment cited is at `:140`, AFTER it. `TileNumberHardLimit` sits at `RecastNavMesh.h:752`,
  and `ARecastNavMesh` opens with `GENERATED_UCLASS_BODY()` at `:571`, a macro that ends in `public:`
  (`RecastNavMesh.generated.h:74-81`), with no specifier between. The `protected:` that comment cited at
  `:490` belongs to the nested `FNavMeshTileData::FNavData`, which closes at `:493`.
  **The larger finding: a `Friend=` on an ENGINE class cannot work here at all.** The transformer is a
  UBT plugin that injects into the UHT-GENERATED header. FactoryGame targets get one —
  `FGCircuitConnectionComponent.generated.h:58`, `FGInventoryComponent.generated.h:153`. The engine's
  own generated headers carry zero and are dated 2026-06-02, because they ship with the installed
  engine and this project's UHT never rebuilds them. Both entries were inert from the day they were
  written. All six remaining entries target FactoryGame classes and all six were verified present in
  the generated headers. The rule is now written at the top of the ini.

- **3. `FFPMNoOwnerRpcGate` was swallowing an engine assertion.** Cancelling `ProcessRemoteFunction`
  skips its whole body, and the first thing in that body under `#if !UE_BUILD_SHIPPING` is
  `checkf(IsInGameThread(), ...)` at `NetDriver.cpp:7821`. The gate now logs an Error once per session,
  naming the actor class and the function, whenever it cancels off the game thread. It logs rather than
  asserts on purpose: reproducing the check would mean FPM owning a crash, and Ant plays Shipping where
  the engine check is compiled out and a log line is the only diagnostic that can exist.

- **4. `FFPMStallSampler::Disarm` ran its four steps in the wrong order.** The comment described
  "unbind first, then join", the code set `bStopping` AFTER the unbind, and unbinding freezes the
  heartbeat. A frozen heartbeat is indistinguishable from a stalled game thread, which is exactly what
  this thread hunts — so the watchdog could wake during teardown and SUSPEND THE GAME THREAD to walk its
  stack. Order is now stop, unbind, join, report. Reporting after the join also means nothing else can
  be inside `Results` while it is read.

- **5. `LastAllowedSweepSeconds` was written twice and read nowhere.** The same dead shape as a counter
  nobody prints. `LogReport` now prints its age, because a 95% cancel rate reads identically whether the
  last real sweep was four seconds or forty minutes ago, and only the second says the library has gone
  quiet. Zero prints as "none allowed yet", not as "0.0 s ago".

- **6. The power probe called a frozen number "peak this session".** Only the 60 s window after load fed
  it, so past that the value stopped moving, and a frozen zero reads exactly like a measured zero. Worse,
  `ReportNow` computed `Max(peak, sample)` for the printout and threw the result away, so a tripped fuse
  caught on demand vanished from the next report. It now stores the maximum and labels its real
  coverage: the window after load plus every `FPM.Power.Report` since, and not the time in between.

- **7. NOT A DEFECT — the reviewer's fix-class count was wrong, `check_structure.py` was right.** 24
  classes derive from `IFPMFix`, and the review counted 21 by listing `Fixes/` and missing the four in
  `Core/` (`FPMGCMeter`, `FPMHitchMeter`, `FPMSaveSettingsInterceptor`, `FPMStallSampler`) while wrongly
  counting `FPMChatRelay`, which is not one. All 24 are armed in `FicsitsPerformanceManager.cpp` — 24
  classes, 24 `FPMFixes::Arm` calls, nothing unwired.

- **Files:** `Config/AccessTransformers.ini`, `Private/Fixes/Vanilla/FPMBlueprintSweepGate.cpp`,
  `Public/Fixes/Vanilla/FPMBlueprintSweepGate.h`, `Private/Fixes/Vanilla/FPMCloneSensor.cpp`,
  `Private/Fixes/Vanilla/FPMPowerWarningProbe.cpp`, `Private/Fixes/Interop/FPMNavMeshCeiling.cpp`,
  `Private/Fixes/Interop/FPMNoOwnerRpcGate.cpp`, `Private/Core/FPMStallSampler.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.9.1 → **0.9.2**, PATCH: repairs to existing surfaces, and
  nothing here is visible with the console closed).
- **Revert:** `git revert` the commit. Undo it if the unsubscribe in (1) turns out to destabilise
  teardown, which would show as a shutdown hang or a crash inside SML's hook arrays.
- **Verified:** build-only. `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`, and that clean compile is itself the receipt for (2), because removing a friend
  a member genuinely needed cannot link. NOT-YET boot-tested.

---

## 2026-08-10 13:45 — CODE — stall sampler: name the game thread's missing 400 ms

- **What:** A new fix, `FFPMStallSampler`. A low-priority watchdog thread reads a per-frame heartbeat
  stamped by the game thread. When a frame passes `FPM.Stall.SampleAfterMs` (default 120) it captures
  the GAME THREAD's callstack with `FPlatformStackWalk::CaptureThreadStackBackTrace` and attributes each
  program counter to its owning MODULE by range test against a table snapshotted at Arm. Reports two
  rankings — top-of-stack (what it was executing) and anywhere-in-stack (who is on the callpath) — both
  with their denominator. `FPM.Stall.Report`.
- **Why:** the 0.8.4 boot narrowed the hitches as far as counting can. 54 of 55 GAME-THREAD BOUND, game
  thread busy ~99.9% of the span, 83-100% matching none of the six cause buckets, render thread idle at
  11 ms while the game thread burned 400. A later window measured worst 449.8 ms, mean 216.9 ms, 7 of 8
  unattributed. The next question is not "how many" but "doing WHAT", and only a stack sample answers it.
- **Module, not function, and that is enough:** a retail install ships no PDBs, so function names are
  not available and this does not pretend otherwise. Addresses resolve by arithmetic against
  `BaseOfImage`/`ImageSize` — no symbol server, nothing that can fail quietly. With 53 mods on the
  server and 124 in her client profile, "14 of 20 samples inside FicsitWiremod" IS the answer.
- **It suspends the game thread, so it is bounded rather than trusted:** at most one sample per stall,
  a minimum gap (`FPM.Stall.MinGapMs`, 250), and a hard session cap (`FPM.Stall.SessionBudget`, 200)
  whose usage is printed beside the results. The module table is snapshotted at Arm on the game thread
  and never re-read while a suspend is in flight — taking a lock the suspended thread might hold is the
  classic profiler deadlock, and the snapshot makes it impossible rather than unlikely.
- **Liveness:** refuses to arm at all if the module table comes back empty, and counts captures that
  returned zero frames separately, so a session where every capture failed cannot read as a quiet one.
- **Limit stated in the report itself:** this samples frames that ALREADY overran, so the percentages
  are a profile of where the game thread is WHEN STUCK, not a general profile.
- **Files:** `Public/Core/FPMStallSampler.h`, `Private/Core/FPMStallSampler.cpp`, `Core/FPMDiag.h` +
  `FPMDiag.cpp` (new `StallSampler` channel — the existing static_assert caught the half-finished
  registration at compile time, exactly as designed), `Private/FicsitsPerformanceManager.cpp`,
  `.uplugin` (0.8.5 → 0.8.6).
- **Bump rationale:** PATCH. Diagnostics, invisible with the console closed.
- **Revert:** remove the `FPMFixes::Arm(FFPMStallSampler::Get())` line, or set
  `FPM.Stall.SessionBudget 0` to stop all sampling while leaving the fix armed.
- **Verified:** `FactoryEditor` + `FactoryGameSteam Win64 Shipping` + `FactoryServer Linux Shipping` all
  Succeeded · `check_structure.py` 23 fixes / 0 / 0 · packaged and payloads verified at 0.8.6 · NOT yet
  boot-tested.

---

## 2026-08-10 13:00 — CODE — vox-review fixes on the work-or-wait split: an off-by-one frame, and three ways it could state a confident wrong cause

- **What:** Four review findings on the 0.8.3 split, fixed before it ever ran.
  1. **BLOCKER — off by one frame.** The game-thread busy time was accumulated in `OnFrameEndGameThread`
     and consumed in `ClassifySpan`. Our ticker runs at `LaunchEngineLoop.cpp:5852`, the `OnEndFrame`
     broadcast at `:5869` — so the span closed BEFORE the current frame's end fired, and the read
     returned the PREVIOUS frame's duration against the CURRENT frame's span. A 700 ms game-thread stall
     gave `SpanMs≈700, GtBusy≈4` and printed `NEITHER THREAD BUSY - gpu/vsync/os`. The one hitch class
     the split exists to name was the one it misnamed, and it pointed at the wrong half of the engine.
     Now read live as `Now - GtFrameStartSeconds`, which is the current frame's work up to the tick on
     the same clock as the span.
  2. **BLOCKER — no liveness proof.** The three-way verdict has a fall-through. With the frame delegates
     dead, both values sit at 0.0, every test fails, and 100% of hitches report a specific confident
     cause that is a lie. Worse than a dead zero: a zero is useless, this certifies the wrong subsystem.
     `GtFramesSeen` now gates the verdict, and a dead split prints `thread split UNAVAILABLE` with an
     explicit "do not read this as a GPU stall".
  3. **BLOCKER, found while fixing 2 — the same failure one level down.** `Side()` is `Any`. On a
     dedicated server the engine loop runs so the game-thread delegates DO fire, but there is no render
     thread, so `OnEndFrameRT` never does. One shared liveness flag would have made every non-game-thread
     server hitch blame `gpu/vsync/os` on a machine with no GPU. The halves are now proved separately.
  4. **HIGH — the capability line was gated on the wrong buckets.** It printed `precaching`, which gates
     `FramesDuringPsoWork`, but the whole line was suppressed once cold creations were non-zero — and
     they will be (100 measured in eleven minutes). Each bucket now states its own capability when it is
     the one sitting at zero.
  Also: `worst frame work` relabelled `worst ON A HITCH`, since it only samples hitching spans; the
  render-thread accumulator documented as "frame time COMPLETED during this span", which is what these
  two delegates can honestly report.
- **Why:** Ant asked for a vox-review of the three PSO/hitch commits and for the findings to be fixed.
  Three of the four are the same smell the review skill names as the most expensive in this project —
  an instrument that does not merely fail, but states a confident wrong cause.
- **Files:** `Public/Core/FPMHitchMeter.h`, `Private/Core/FPMHitchMeter.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.8.3 → 0.8.4, all three version fields).
- **Spec note carried forward, not silently dropped:** Ant said *"Fpm should hook whatever it needs. We
  need all the control we can get"*, and no hook was written. The review flagged that as reinterpreted
  scope. `FDynamicRHI::RHICreateGraphicsPipelineState` is pure virtual (`DynamicRHI.h:414`), reachable
  through `GDynamicRHI`, cold-path only, and SML's virtual hook needs no UObject — it is ready to write.
  The boot below decides it: if `LogPSOHitching` produces its lines, the deferral was right.
- **Revert:** the split as a whole reverts by removing the four `FCoreDelegates` subscriptions and the
  verdict block in `ClassifySpan`.
- **Verified:** `FactoryEditor Win64 Development` and `FactoryGameSteam Win64 Shipping` both
  `Result: Succeeded` · `check_structure.py` 22 fixes / 0 errors / 0 warnings · boot test follows.

---

## 2026-08-10 11:55 — CODE — every hitch now says WHERE the time went, not only what happened during it

- **What:** The meter subscribes to `FCoreDelegates::OnBeginFrame` / `OnEndFrame` (game thread) and
  `OnBeginFrameRT` / `OnEndFrameRT` (render thread), and reports a three-way verdict per hitch:
  **game-thread bound**, **render-thread bound**, or **neither thread busy (gpu/vsync/os)**. The raw
  numbers print beside the verdict so it can be checked rather than trusted. The window summary carries
  the tally and the worst frame work seen on each thread.
- **Why:** Every bucket in this meter asks what HAPPENED during a span. None asked the prior question.
  The span is wall clock between core-ticker ticks, so it contains the game thread's own work AND
  everything it then waits on. A 723 ms hitch had two opposite explanations the meter could not tell
  apart — the game thread did 723 ms of work, or it did 4 ms and waited 719 ms. Her logs are full of the
  case that needs it: `2 hitch(es) ... worst 212.5 ms ... 0 flush, 0 SYNC load, 0 GC, 0 PSO | 2
  UNATTRIBUTED (100%)`. This does not name those, but it halves the search space for all of them, and
  the third verdict is a real finding no existing bucket could produce.
- **Why the subtraction is valid:** the core ticker driving `Tick()` runs at `LaunchEngineLoop.cpp:5852`,
  BETWEEN the `OnBeginFrame` broadcast at `:5462` and the `OnEndFrame` broadcast at `:5869`. So
  `OnBeginFrame -> OnEndFrame` is the game thread's own frame work on the same clock as the span. All
  four delegates are plain CORE_API `FSimpleMulticastDelegate` (`CoreDelegates.h:262-274`) broadcast from
  the main engine loop with no editor guard.
- **Kept orthogonal to attribution, deliberately.** A hitch can be game-thread bound AND unattributed.
  That pairing is the most useful thing the meter can say about a hitch it cannot name, and folding the
  verdict into `bAttributed` would have destroyed it by making every hitch look explained.
- **Threads and types:** the render-thread pair accumulates across threads, so both its fields are
  atomic, and the accumulator is `int64` MICROSECONDS rather than `std::atomic<double>` — floating-point
  `fetch_add` is a C++20 addition with uneven support, and integer microseconds are exact at these
  magnitudes with no compare-exchange loop.
- **One race accepted and documented rather than closed:** `Remove()` on the render-thread delegates runs
  from the game thread and is not ordered against an in-flight broadcast. Safe because the meter is a
  function-local static with process lifetime, and the only possible late write is into an accumulator
  nothing reads after `LogSummary` has already run. `FlushRenderingCommands()` during teardown would be
  the riskier option for a nil consequence. ⚠ The first version of this comment claimed an ordering the
  code did not implement; corrected before commit.
- **Files:** `Public/Core/FPMHitchMeter.h`, `Private/Core/FPMHitchMeter.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.8.2 → 0.8.3, all three version fields).
- **Bump rationale:** PATCH. Diagnostics, invisible with the console closed, no behavior change.
- **Revert:** Remove the four `FCoreDelegates` subscriptions and the verdict block in `ClassifySpan`.
  Undo if the per-frame delegate cost ever measures as non-free — it is four multicast broadcasts per
  frame that the engine already performs whether or not anyone is listening.
- **Verified:** build-only (`FactoryEditor Win64 Development`, `Result: Succeeded`, 25.98 s) ·
  `check_structure.py` 22 fixes / 0 errors / 0 warnings · NOT-YET boot-tested.

---

## 2026-08-10 11:40 — CODE — the engine's own per-PSO timing switched on, because it was compiled in and merely silent

- **What:** `Arm()` raises the engine log category `LogPSOHitching` to Verbose, and `Disarm()` puts it back
  to its declared `Log` default. Guarded by `FPM.Pso.EngineHitchLog` (default 1) and never armed on a
  dedicated server. The game then prints one line per runtime pipeline build over
  `r.PSO.RuntimeCreationHitchThreshold` ms, carrying the duration, the pipeline name and the precache
  status:
  `Runtime graphics PSO creation hitch (%.2f msec) for %s (precache status: %s)`.
- **Why:** Ant, 2026-08-10: *"Fpm should hook whatever it needs. We need all the control we can get"*.
  The intended target was a hook on `FDynamicRHI::RHICreateGraphicsPipelineState`, which is pure virtual
  (`DynamicRHI.h:414`) and reachable through `GDynamicRHI` — SML's virtual hook resolves the vtable from a
  member-pointer thunk and needs no UObject, so it was viable. Checking the engine first found something
  better. `PipelineStateCache.cpp:238-279` already times every runtime creation and already logs duration,
  name and precache status. That is strictly more than the hook would have produced, for no hook.
- **The fact that had to be checked, not assumed:** the Verbose line is COMPILED INTO the shipped binary.
  `USE_LOGGING_IN_SHIPPING` is 1 in this build's own SharedDefinitions header, so `NO_LOGGING` is 0
  (`Misc/Build.h:320`), and `COMPILED_IN_MINIMUM_VERBOSITY` defaults to `VeryVerbose`
  (`LogMacros.h:81-82`) with no override anywhere in this project's shipping definitions. The line exists
  and was suppressed only at runtime by the category's `Log` default.
- **By name, not by symbol:** the category is `DEFINE_LOG_CATEGORY_STATIC` in another translation unit, so
  there is no symbol for `UE_SET_LOG_VERBOSITY`. `FSelfRegisteringExec::StaticExec` is CORE_API and `LOG`
  is handled by `FLogSuppressionImplementation::Exec_Runtime` (`LogSuppressionInterface.cpp:589-591`) —
  `Exec_Runtime`, so it is present in Shipping. Categories self-register by name, so static linkage does
  not block it.
- **Why not the hook, and when it should become one:** the hook would add millisecond attribution on the
  in-game overlay, which this does not. The in-game half is already served by the cold-creation counter
  added in the previous entry, so the hook buys only overlay precision — at the price of sitting on the
  render thread inside the renderer's pipeline creation path. Worth doing when a boot shows the log half
  is not enough. Not before.
- **Zero residue:** a category's runtime verbosity is in-memory only. No ini is written. `Disarm()`
  restores unconditionally rather than re-reading the cvar, so flipping the cvar off mid-session cannot
  strand the category at Verbose.
- **Files:** `Private/Core/FPMHitchMeter.cpp`, `FicsitsPerformanceManager.uplugin`
  (0.8.1 → 0.8.2, `VersionName` / `SemVersion` / `RemoteVersionRange`).
- **Bump rationale:** PATCH. Diagnostics, invisible with the console closed, no behavior change.
- **Revert:** Set `FPM.Pso.EngineHitchLog 0`, or remove the two `FPMSetEnginePsoHitchLogging` calls. Undo
  if the line proves too noisy in a real session — measured expectation is about nine lines per minute,
  from the 100 hitches in eleven minutes in her 03:27 log.
- **Verified:** build-only (`FactoryEditor Win64 Development`, `Result: Succeeded`, 26.47 s) · NOT-YET
  boot-tested. ⚠ The boot test for this one is trivial and worth doing first: grep the client log for
  `LogPSOHitching`. If the Verbose lines appear, the whole PSO question becomes readable offline.

---

## 2026-08-10 09:50 — CODE — PSO attribution widened from one mechanism to three, and the old bucket shown to be a tautology

- **What:** The hitch meter watched one of the three ways a pipeline state object costs time in UE 5.6.
  It now watches all three.
  1. **Cold PSO creation (new, and the important one).** Subscribes to
     `FPipelineFileCacheManager::OnPipelineStateLogged()`, which fires once per pipeline that was not in
     the cache and had to be built during play. Counted per span, split graphics / compute / ray tracing,
     and added to the attributed set. This is an EVENT inside the span, so it sits beside the flush and
     sync-load terms rather than beside the run flag.
  2. **Async PSO work in flight (new).** Samples `GetNumActivePipelinePrecompileTasks()` and
     `NumActivePrecacheRequests()` once per span. The second one covers `r.PSOPrecache`, a whole mechanism
     the meter could not see before. Reported as a rate against its own denominator, with per-window
     peaks, for the reason the run bucket already documents.
  3. **The existing precompile-run flag**, kept and now explained rather than bare.
  Adds `FPM.Pso.Report`, which prints the capability of each bucket before it prints any count.
  Also repairs a pre-existing defect found while adding this: the `!bPrimed` branch discarded accumulated
  flushes so a loading screen could not be blamed on the first playable frame, but it never discarded the
  sync-load or GC counters. All four in-frame counters are discarded there now.
- **Why:** Ant, 2026-08-10: *"Pso stuff need to be even wider"*. Measurement says the old bucket could
  never have answered anything in game. From her own 0.8.0 client log, the precompile runs finish at
  07:05:20 and the first hitch window opens at 07:06:16, so `bPsoRunActive` is false for the entire
  playable session and `0 were during a PSO precompile` is a tautology, not a finding. Meanwhile the
  engine's own `LogPSOHitching` recorded `100 PSO creation hitches so far (79 graphics, 21 compute). 10 of
  them were precached.` in about eleven minutes of her 03:27 session. That is 90 cold misses over 20 ms
  each, it matches her report of hitches while moving through the world, and FPM counted none of it.
  The engine's counters for those are file-scope statics with no getter and a `STATS` build gate, so the
  count is unreachable from a mod. The cause is reachable, and that is what this subscribes to.
- **Gate verified before wiring, not after:** `ReportNewPSOs()` reads `r.ShaderPipelineCache.ReportPSO`,
  default `PIPELINE_CACHE_DEFAULT_ENABLED` = `(!WITH_EDITOR)` = 1 in a retail client
  (`PipelineFileCache.h:18`). Corroborated on disk rather than from the default alone:
  `Saved/FactoryGame_PCD3D_SM6.upipelinecache` is 2.6 MB, written 2026-08-10 05:33, and the log reports
  that user cache holding 5464 entries. All four engine calls are null-safe on a NullRHI dedicated server,
  checked because this fix is `Side() == Any`.
- **Files:** `Public/Core/FPMHitchMeter.h`, `Private/Core/FPMHitchMeter.cpp`,
  `FicsitsPerformanceManager.uplugin` (0.8.0 → 0.8.1, `VersionName` / `SemVersion` /
  `RemoteVersionRange`). No `Build.cs` change: `RHI` is already a private dependency and both new headers
  are included only from `Private/`.
- **Bump rationale:** PATCH, not MINOR. By this file's own player-notice test, diagnostics and probes are
  invisible with the console closed. Nothing here changes behavior.
- **Revert:** Remove the `PsoLoggedHandle` subscription in `Arm()` and its removal in `Disarm()`, the
  `OnPsoCreated` body, and the two level samples in `ClassifySpan`. Undo if the per-frame atomic reads
  ever measure as non-free, or if the ±1 frame marshalling delay proves to misattribute in practice.
- **Verified:** build-only (`FactoryEditor Win64 Development`, `Result: Succeeded`, 28.36 s) ·
  `check_structure.py` 22 fixes / 0 errors / 0 warnings · NOT-YET boot-tested.

---

## 2026-08-10 08:40 — VERSION — 0.7.0 → 0.8.0

- **What:** MINOR bump. `VersionName` / `SemVersion` / `RemoteVersionRange` in the `.uplugin`, and the
  `Description` rewritten to the real roster — **20 armed fixes**, up from the eighteen 0.7.0 claimed.
- **Why MINOR and not PATCH.** The house test is *"would a player notice this while just playing, with
  the console closed?"* Most of this wave fails that test on its own: the crash stamp, the power probe
  and the distance-field audit are console- and log-only, and `FPMEnclosure` currently has no consumers
  so it does not run at all. **One item passes it and carries the bump alone: the schematic null-guard.**
  It changes a vanilla answer and prevents the largest crash-to-desktop class in the dump corpus — a
  player notices not crashing.
- **What the wave carries** (15 commits since `829d226`):
  - `schematic-null-guard` — **the only player-visible one.** Refuses schematic access when the game is
    about to read a null event subsystem. Blast radius measured at 18 FICSMAS schematics of 1,455, so it
    is structurally incapable of touching a HUB tier or a MAM node.
  - `power-warning-probe` — reads circuit state to answer whether the "Fuse Blown" popup is lying.
    Deliberately no hook: the emitter is a `BlueprintNativeEvent` with an empty native body.
  - `distance-field-audit` — counts instanced meshes the renderer cannot see. Audit only; the repair is
    behind `FPM.DistanceField.Repair`, default 0, because it adds renderer work.
  - `FPMCrashStamp` — a dump now names FPM's version, side, roster and hook count without the log.
  - `FPMEnclosure` — the one shared "am I inside" check. **No consumers yet, so it never runs.**
  - Repo structure: README, LICENSE, CONTRIBUTING, `tools/check_structure.py`, the real NOX art, and
    four `.uplugin` defects fixed including a `GameVersion` that had been stale since July.
  - A `vox-review` pass over all of it: one blocker, one high, three medium, all fixed.
- **Files:** `FicsitsPerformanceManager.uplugin` (4 fields).
- **Revert:** restore the four fields to `0.7.0`. Only while 0.8.0 is unpackaged — once a build ships
  under this number it is spent.
- **Verified:** `check_structure.py` — 20 fixes, 0 errors, 0 warnings. Module builds clean. **NOT
  packaged, NOT deployed, NOT boot-tested.**
- **⚠ SML enforces exact-equality parity on `RequiredOnRemote` mods.** `RemoteVersionRange` is now
  `=0.8.0`, so client and server must be deployed TOGETHER or the join is refused. The deployed pair is
  still the 04:53 packages, which predate every commit in this wave.

---

## 2026-08-10 07:10 — SPEC — repository structure brought to the community standard, and a checker so it stays there

- **What:** Added `README.md`, `LICENSE` (GPL-3.0 text) and `CONTRIBUTING.md`. Fixed four `.uplugin`
  defects. Added `tools/check_structure.py`. Extended `.gitignore`. Rewrote the reader-facing prose in
  Simplified Technical English.
- **Why:** Ant asked whether the repo follows the community structure. It did not. FPM1 shipped a
  README, a LICENSE and a CONTRIBUTING file. **The rewrite dropped all three and nobody noticed for two
  days.** The missing LICENSE is a real compliance defect, not an omission of courtesy: every source
  file claims GPL-3.0 and the license text was not in the repo.
  Four `.uplugin` defects, each verified against Alpakit or the crash-free build:
  1. `GameVersion` read `>=491125`. The tested build is CL **495413**. A written audit flagged this on
     2026-07-17 and the rewrite carried the stale value through unfixed.
  2. `AbstractInstance` had no `"BasePlugin": true`. Alpakit treats a dependency without that flag as a
     ficsit.app mod and tries to version-resolve it (`ModMetadataObject.cpp:170-180`).
  3. `Wwise` was not declared at all, although two shipped fixes call `UAkGameplayStatics`.
  4. `Cartograph` was not declared, although `FPMTexturePoolGuard.cpp:83` detects it to size the pool.
  The `Description` also still said "Nine targeted repairs" while eighteen fixes were armed.
- **Files:** `README.md`, `LICENSE`, `CONTRIBUTING.md`, `tools/check_structure.py` (all new),
  `FicsitsPerformanceManager.uplugin`, `.gitignore`.
- **Revert:** delete the new files and restore the `.uplugin` fields. Do not revert `GameVersion`.
- **Verified:** `python tools/check_structure.py` reports **18 fixes, 0 errors, 0 warnings**. The
  `.uplugin` re-parses as valid JSON. Module builds clean.
- **★ THE CHECKER IS THE POINT, NOT THE THREE FILES.** A written checklist already covered every one of
  these defects. It sat in `10-DOCS` and nobody ran it. So the checklist is now code that runs in the
  repo it checks. It verifies the required files, the `.uplugin` fields against each other and against
  the tested build, all four contract members on every fix header, that every fix is actually armed, the
  diagnostics table (whose own `static_assert` checks count only, by its own admission), the no-network
  hard rule, the license headers, and that no hook skips the ledger.
- **★ AND IT CHECKS PREDECESSOR COVERAGE, WHICH IS THE MISTAKE THAT KEEPS HAPPENING.** It reads FPM1's
  registration list and fails when a fix there has no recorded disposition in FPM2 — carried, or dropped
  by ruling. That turns "we forgot" and "we decided" into two visibly different states. The Wwise gate
  was orphaned for two days; the pattern match it uses is deliberately `Register<Anything>()` and not
  `Register<Anything>Fix()`, because the narrower grep is the one that once reported "FPM1 has only 4
  fixes" and hid the orphan.
- **Prose:** README, CONTRIBUTING and the `.uplugin` description are now written in ASD-STE100
  Simplified Technical English. Active voice, one instruction per sentence, no semicolons, no
  contractions, short common words. Ant: *"we also need to make the texts human readable."*
- **URLs:** `CreatedByURL`, `DocsURL` and `SupportURL` now point at
  `github.com/DegradingAnt/Ficsit-performance-manager-`, per Ant's ruling that FPM2 is the same mod
  remade and goes where FPM1 went. There is no project Discord yet, so the README names none.

---

## 2026-08-10 06:30 — CODE — P3.10(a) schematic null-guard, built against the corpus and NOT against the design line

- **What:** New `FFPMSchematicNullGuard` (`Fixes/Interop/FPMSchematicNullGuard.{h,cpp}`) on
  `UFGSchematic::CanGiveAccessToSchematic`. Refuses (`Scope.Override(false)`) when the schematic
  declares relevant events **and** `AFGEventSubsystem::GetEventSubsystem(worldContext)` is null. Keeps
  the design's null-class / null-CDO checks beside it. New `SchematicGuard` diag channel, a BEHAVIOUR
  cvar `FPM.SchematicGuard`, and `FPM.SchematicGuard.Status`.
- **Why:** ⚠ **THE DESIGN'S SPEC FOR THIS FIX IS REFUTED BY OUR OWN CRASH DUMPS.** P3.10(a) asks for
  "a null/CDO check on the TSubclassOf argument" with "6/6 dump coverage". Re-derived from the 57 dumps
  on disk 2026-08-10: **19** carry `UFGSchematic::CanGiveAccessToSchematic` as **frame 0** under
  `EXCEPTION_ACCESS_VIOLATION reading 0x2c0` — and FPM1 **0.58.52 already shipped exactly that check**
  (`Scope.Override(false)` on `!InClass || GetDefaultObject(false)==nullptr`), with **14** of the 19
  dumps landing on builds after it (.53 .55 .61×2 .62×2 .66 .68 .71×6). The stacks show FPM's
  pass-through frames BELOW vanilla's frame 0 — the guard tested the arguments, found them sound, and
  handed off. FPM1's own file already recorded the conclusion: *"Every argument was sound. The null
  lives INSIDE vanilla's body."* Shipping the specified check verbatim would have shipped a fix already
  proven inert, carrying a receipt for a class it has never caught.
  **The real condition, from three witnesses:** `FGSchematic.h:158` says the function "checks for
  events"; `FGEventSubsystem.h:127-136` shows `Get()` returning null and `IsEventActive` reading
  `mCurrentEvents` off `this` (a member read off null = `0x2c0`); and **12 of the 19 dumps record
  `GameStateName = FGMainMenuState`** — the main-menu world, which has no event subsystem. The other
  seven are in-world with uptimes clustering at 32-103 s, i.e. the join transition.
- **Files:** `Fixes/Interop/FPMSchematicNullGuard.{h,cpp}` (new), `Core/FPMDiag.{h,cpp}` (channel +
  cvar + array + name switch), `FicsitsPerformanceManager.cpp` (include + Arm).
- **Revert:** `FPM.SchematicGuard 0` disables the refusal at runtime with no rebuild; to remove
  entirely, drop the `FPMFixes::Arm(FFPMSchematicNullGuard::Get())` line.
- **Verified:** build-only (`Build.bat FactoryEditor -Module=FicsitsPerformanceManager`, Succeeded,
  23.69 s). NOT boot-tested.
- **⚠ ARM ORDER IS CORRECTNESS, NOT STYLE.** It arms AFTER `FFPMSchematicProbe`, because
  `TCallScope::Override` sets `bForwardCall = false` and thereby skips every handler registered later
  (`NativeHookManager.h:216-228`). Guard-first would silence the probe on exactly the calls worth
  observing.
- **⚠ WHY IT REFUSES SO NARROWLY.** A blanket "refuse whenever the subsystem is null" can refuse a
  grant vanilla would have answered fine — the milestone-lockout Ant hit on FPM1 0.58.51 (*"i cant
  input stuff into the HUB for milestones"*). That file's own ruling stands: *"the crash was survivable
  by rebooting, an unusable HUB is not."* So it refuses only the combination that must dereference.
- **★ THE NARROWING IS FALSIFIABLE ON PURPOSE.** A null subsystem on a schematic with NO relevant
  events is passed through and counted as `PassedEventless`. If a `0x2c0` crash lands while that
  counter is non-zero, the reasoning here is dead by measurement and the guard must widen to refuse on
  a null subsystem alone. `FPM.SchematicGuard.Status` prints it and says so in the log.
- **Origin status is `Guard`, deliberately NOT `OriginNamed`** despite three converging witnesses:
  vanilla's body has not been read, so this is a strong lead, not a receipt.

---

## 2026-08-10 00:40 — CODE — Wwise server audio gate: re-port of a fix the rewrite orphaned

- **What:** New `FFPMWwiseServerGate` (`Fixes/Interop/FPMWwiseServerGate.{h,cpp}`). On a DEDICATED
  SERVER ONLY, cancels `UAkGameplayStatics::StopActor`. New `WwiseGate` diag channel and
  `FPM.WwiseGate.Report`.
- **Why:** `AkGameplayStatics.cpp:966-979` fetches the audio device first and returns if it is null,
  logging `Could not retrieve audio device.` A dedicated server has no audio device, so the call is a
  **guaranteed no-op whose only effect is the warning** — cancelling it there is behaviour-identical
  minus the log write. Measured **681** occurrences in the 2026-08-09 server session; it is one of
  three repeating lines that are together 23% of a 61,687-line log, and a log you have to wade through
  hides the crash callstack you were actually looking for.
- **Files:** `Fixes/Interop/FPMWwiseServerGate.{h,cpp}` (new), `Core/FPMDiag.{h,cpp}` (channel + cvar +
  name switch), `FicsitsPerformanceManager.cpp` (include + Arm).
- **Revert:** drop the `FPMFixes::Arm(FFPMWwiseServerGate::Get())` line.
- **Verified:** build-only (`Build.bat FactoryEditor -Module=FicsitsPerformanceManager`, Succeeded).
  NOT boot-tested — the count can only be confirmed on a deployed server.
- **⚠ THIS IS THE THIRD FIX THE REWRITE ORPHANED.** FPM1 had it as
  `RegisterWwiseServerAudioGate` (`FicsitPerformanceManager.cpp:1480-1510`); FPM2 shipped without it
  and nothing noticed until its warnings turned up while reading an unrelated crash log. REBUILT, not
  copied, and its central claim was re-verified from the Wwise source rather than trusted from the old
  comment — which cited 3,164 warnings/session, a figure from ITS session, not ours.
- **Client safety:** the guard is at REGISTRATION, not per call. A client's audio device is real and
  `StopActor` must run; `Arm()` returns early there, so a client installs no hook at all and there is
  no per-call branch for a later edit to get wrong.

---

## 2026-08-09 22:55 — VERSION — 0.6.0 → 0.7.0

- **What:** MINOR bump. Ships the wire null guard (new fix), the hitch meter's PSO bucket and
  unattributed rate, the texture-pool guard repair, the widened clone sensor and the RHI memory
  readout. `VersionName` / `SemVersion` / `RemoteVersionRange` in the `.uplugin`. The StartupModule
  banner needs no edit — unlike FPM1 it reads `GetDescriptor().VersionName` at runtime
  (`FicsitsPerformanceManager.cpp:44`), so it cannot drift from the descriptor.
- **Why:** ⚠ THE BUMP IS THE FIX FOR A LIVE TRAP, not bookkeeping. Source and DEPLOYED both read
  `0.6.0` while source carried five changes the deployed build did not. `sf-savestate`'s first
  verification check is "source `.uplugin` == deployed `.uplugin`" — it was PASSING FALSELY, which is
  exactly the state where a boot measures one build and the number gets attributed to another.
- **Files:** `FicsitsPerformanceManager.uplugin` (3 fields).
- **Revert:** restore the three fields to `0.6.0`. Do it only if 0.7.0 is never packaged; once a
  build ships under this version the number is spent.
- **Verified:** build-only. NOT boot-tested, NOT packaged, NOT deployed.
- **⚠ SML enforces exact-equality parity on `RequiredOnRemote` mods.** `RemoteVersionRange` is now
  `=0.7.0`, so client and server must be deployed TOGETHER or the join is refused. The deployed pair
  is currently 0.6.0/0.6.0 and consistent; do not deploy one side alone.

---

## 2026-08-09 22:40 — CODE — wire null guard: the autosave crash that took the dedicated server down

- **What:** New `FFPMWireNullGuard` (`Fixes/Vanilla/FPMWireNullGuard.{h,cpp}`). Sweeps
  `UFGCircuitConnectionComponent::mWires` for NULL entries at every world save AND at world load,
  names every owning actor, and compacts the nulls out. New `WireGuard` diag channel; new
  `FPM.WireGuard.Repair` cvar (default 1; 0 = report only, keeps the naming half);
  `FPM.WireGuard.Sweep` and `FPM.WireGuard.Report` console commands.
- **Why:** 2026-08-09 21:42:41 local the DatHost server SIGSEGV'd inside its autosave with two
  players connected — `UClass::ImplementsInterface` on a null class, from
  `FFastSaveReferenceCollector::HandleObjectReference` (`FGSaveSession.cpp:2577`). Ant and SunFry
  froze for 26 s and quit. Autosaves had been failing unnoticed, so this was data loss, not just a
  crash. Root cause was a blueprint pack authored on a dev test map (see the WORLD/TEST entry below);
  the guard is the belt to that braces, and it holds for any future source of the same invalid state.
- **Files:** `Fixes/Vanilla/FPMWireNullGuard.{h,cpp}` (new), `Core/FPMDiag.{h,cpp}` (channel + cvar +
  name switch), `Config/AccessTransformers.ini` (friends for `UFGCircuitConnectionComponent` and
  `UFGSaveSession`), `FicsitsPerformanceManager.cpp` (include + Arm).
- **Revert:** drop the `FPMFixes::Arm(FFPMWireNullGuard::Get())` line. The guard installs one hook and
  owns no state anything else reads.
- **Verified:** build-only (`Build.bat FactoryEditor -Module=FicsitsPerformanceManager`, Succeeded).
  **NOT boot-tested — no null has been observed through this instrument yet**, and until one is, the
  sweep is unproven against real damage.
- **⚠ SHIPPED WRONG ONCE, HOURS APART.** `a13456f` swept only at `OnWorldLoad` while its commit
  message claimed it swept "before the autosave walks them". It did not: the nulls arrive when a
  blueprint is PASTED, mid-session, long after the load sweep. `6186b47` added the
  `UFGSaveSession::SaveWorldEndOfFrame` hook so the sweep lands on the frame the crash callstack
  actually names. The lesson is the cheap one — read the commit message back against the code.
- **Rejected on inspection:** hooking `FFastSaveReferenceCollector::HandleObjectReference` itself. It
  is a plain `FReferenceCollector` subclass (`SaveCollectorArchive.h:12`) so there is no CDO sample
  for `SUBSCRIBE_METHOD_VIRTUAL`, its `.cpp` is one of the autogenerated stubs, and it runs once per
  reference across the whole save graph.

---

## 2026-08-09 21:20 — CODE — hitch meter: PSO bucket with its own denominator, and the unattributed rate

- **What:** Fifth attribution bucket on `FFPMHitchMeter` — PSO precompile, maintained by
  `FShaderPipelineCache`'s Begin/Complete delegates — plus `FramesDuringPso` as its denominator and
  an UNATTRIBUTED count printed as a percentage of hitches. Per-hitch lines now carry
  `| during a PSO precompile run` and `| UNATTRIBUTED` so the anonymous ones are greppable.
- **Why:** Ant: *"I'd like to see if we can get the low 1% closer to the main fps."* Her 0.6.0 overlay
  read `21 hitch(es) ... 0 async-load flush, 0 SYNC load, 0 GC` — every hitch outside every bucket.
  The unattributed RATE is the number that has to fall; without it a new bucket looks like progress
  whether or not the anonymous share moved.
- **Files:** `Core/FPMHitchMeter.{h,cpp}`.
- **Revert:** the buckets are additive; removing the four counters and their report lines restores
  the previous output exactly.
- **Verified:** build-only. NOT boot-tested.
- **⚠ Three things I had banked about those delegates were wrong**, each corrected from engine bytes:
  they fire on the RENDER thread (`FShaderPipelineCache : FTickableObjectRenderThread`,
  `ShaderPipelineCache.h:78`); the Complete delegate's `Seconds` is a run TOTAL of compile time
  (`:1204`, reset `:1826`), not a frame duration, so it is session context only; and
  `NumPrecompilesRemaining()` returns a wall-time DECAY ESTIMATE rather than a task count whenever
  `MaxPrecompileTime > 0` (`:831-835`), which is why polling was rejected.

---

## 2026-08-09 17:20 — VERSION — 0.5.7 → 0.6.0

- **What:** MINOR bump. Ships everything since 0.5.7: the P1.3 SaveSettings interceptor, the P3.2/3.4/3.5/3.9
  fixes, the chat relay, `FPM.Support`, the §7.10 CL drift watch and the §7.12 parity self-check.
- **Why MINOR, tested against this file's own rule rather than assumed** — "would Ant notice this while
  just PLAYING, with the console closed?" That rule exists because the call was gotten wrong TWICE on this
  same day, both times bumping MINOR for console-only work. Three things here pass it honestly:
    · **P3.4** raises the navmesh tile ceiling, so local fauna can path the whole map to reach you. Visible.
    · **P3.9** fixes a real bug where the zipline volume could never return to vanilla once lowered. Audible.
    · **the chat relay** puts FPM messages in the chat window, which is a new on-screen surface.
  Plus the crash-class fixes (P3.2's ~1,900–2,550 averted asserts per server start). `FPM.Support` and the
  drift watch on their OWN would have been a PATCH — they are console work. They ride along; they do not
  justify the bump.
- **Files:** `FicsitsPerformanceManager.uplugin` (VersionName, SemVersion, RemoteVersionRange =0.6.0).
  No source bump needed: FPM2's boot banner reads `GetDescriptor().VersionName` at runtime, so the old
  mod's "the version string appears twice and one copy lies" trap does not exist here.
- **Revert:** `git tag -d 0.6.0` and restore the three descriptor fields to 0.5.7. Undo if the packaged
  artefact fails its byte verification, or if a boot shows the new diagnostics misbehaving.
- **Verified:** build-only (`Result: Succeeded`). NOT boot-tested — that is the very next step, and it is
  the Phase 1 verification boot (P1.5 legs A+B, P1.4's residue drill) that Phase 1 has been waiting on.

---

## 2026-08-09 15:52 — CODE — P3.9 zipline volume: ported, and a real bug fixed on the way

- **What:** hooks `AFGEquipmentZipline::Equip` and sets the per-actor Wwise output-bus volume from
  FPM's own cvar `FPM.Zipline.Volume` (default `1.0` = vanilla, writes nothing).
- **⚠ THE OLD VERSION COULD NOT RETURN TO VANILLA.** Its guard was
  `if (!Self || GFPMZiplineVolume >= 0.999f) { return; }` (`FPMZiplineAudio.cpp:66`) and that `return`
  precedes the **only** call to `SetOutputBusVolume`. So `1.0` can never reach the write: lower the
  volume, then set it back, and the write is skipped. If the bus value persists on the actor —
  unverified, but the reason to fix it either way — vanilla is unreachable until a restart. This version
  remembers it has written and then always writes, including `1.0`, so undoing works.
- **Why a cvar and not `US_ZiplineVolume`:** that asset is FGGameUserSettings-backed, so a write is
  re-applied every boot with or without the mod. Permanent residue. Ours is declared by the module and
  gone when it unloads. Phase 4's settings surface will drive it with no change here.
- **Why per-actor and not a Wwise bus:** there is no per-system bus (`Master_Audio_Bus → gameMix →
  _reverbSends`) and no zipline RTPC. Lowering `gameMix` quiets the whole game, which is what the
  vanilla slider already does.
- `NeverOnDedicatedServer` by contract, not a hand-rolled early return, so the skip is logged.
- **Files:** `Public/Fixes/Interop/FPMZiplineVolume.h`, `Private/Fixes/Interop/FPMZiplineVolume.cpp`,
  `FicsitsPerformanceManager.Build.cs` (`AkAudio` re-enabled — Ant: *"the wwise is fine to depend on
  since its part of vanilla"*), `FPMDiag.*`, `FicsitsPerformanceManager.cpp`.
- **Verified:** build-only — `Result: Succeeded`.

---

## 2026-08-09 15:35 — CODE — P3.5 HUD hook guard: strips one descriptor, not a mod's whole asset

- **What:** hooks `UBlueprintHookManager::RegisterBlueprintHook` and removes only descriptors targeting
  `Widget_PlayerHUD::Construct`, from assets on a known-crashing list. Everything else that hooks the
  HUD is allowed through and **named** in the log.
- **Why:** injected code in that Construct asserts on every death and vehicle exit —
  `Widget_PlayerHUD_C:ExecuteUbergraph_Widget_PlayerHUD:10000000C → execAddMulticastDelegate → Fatal`.
- **This guard was too broad twice before and Ant paid for both.** v1 cancelled every hook targeting the
  widget; v2 cancelled KPrivateCodeLib's whole asset — and that library sits under KAPI/KBFL/KUI, so one
  `Cancel()` removed a family's HUD contribution: *"some modded UI doesnt close on escape and parts of
  their UI dosnt exist."*
- **Refinements:** exact function-name match instead of `Contains("Construct")`, which would also match
  `ReconstructWidget`/`PostConstruct` · contract side gate instead of a hand-rolled server return · §3.3
  counters incl. a **cancelled** count that should read zero · strip/cancel lines deliberately NOT
  diag-gated · the old mod's second hook (a `UOverlay::AddChildToOverlay` census) NOT carried, because
  the design says rebuild narrow.
- **Note:** v3 shipped on the old mod in 0.58.72, so if mod UI is still missing, this is likely not the
  cause and needs its own origin hunt.
- **Verified:** build-only — `Result: Succeeded`.

---

## 2026-08-09 15:20 — CODE — P3.4 navmesh tile ceiling: writes the PLACED actors and reads back

- **What:** at world load, walks `TActorIterator<ARecastNavMesh>`, raises `TileNumberHardLimit` to
  524288, and **reads the value back**, reporting a did-not-stick count.
- **Premise, receipted three ways:** register R5 (`TileNumberHardLimit=65536` against 306,440 tiles
  needed) · the old mod's analysis (the generator clamps, so ~21% of the map has navmesh) · **the engine
  itself**, in the live server log: `serialized maxTiles (65536, 16 bits) vs calculated maxtiles
  (306440, 19 bits)`.
- **⚠ A STRAIGHT PORT WOULD SHIP A LIE.** The old version wrote six CDOs at startup and logged
  `65536 -> 524288`; twenty-two seconds later the engine still reported 65536. Its own caveat predicted
  it — a placed actor's serialized value beats the CDO — and nobody checked.
- **So:** per-instance write **with read-back**; enumeration by base class rather than six hardcoded FG
  subclasses; **zero actors found is logged loudly**, because the open question is whether CONSTRUCTION
  runs before the generator reads the ceiling and this refuses to assume it; the CDO write is dropped
  entirely; failure lines are not diag-gated.
- `ChokePointRepair`, not `OriginNamed` — we raise a vanilla cap, we do not fix why it exists.
- **VERIFY:** the maxTiles warning should be **absent** from the next server nav log. That is a *server*
  signal and needs the DatHost restart.
- **Files:** the two new fix files, `Config/AccessTransformers.ini` (friends `ARecastNavMesh` to the FIX
  class, per this repo's convention), `FPMDiag.*`, `FicsitsPerformanceManager.cpp`.
- **Verified:** build-only — `Result: Succeeded`.

---

## 2026-08-09 15:12 — CODE — P3.2 rail GetOpposite guard: ported and refined

- **What:** reproduces vanilla's own precondition — a track exists AND this component is one of its two
  connections — and returns `nullptr` instead of letting `Assertion failed: GetTrack()->GetConnection(1)
  == this` abort the server. A properly wired connection is forwarded untouched.
- **Measured on the old mod across the 12-log DatHost corpus:** 1,900–2,550 averted asserts **per server
  start**, in every one of 11 sessions, **23,450 total**, arriving in a burst — counters #50 to #950
  inside 26 ms.
- **Refinements:** splits **hologram** owners (expected, sampled) from **placed** ones (should be
  impossible → unthrottled Error) — the old guard threw both into one 1-in-50 throttle, which could hide
  the only real instance behind two thousand expected ones · §3.3 denominator pair so a guard that goes
  quiet after the upstream fix can be retired on evidence · `FPM_SUBSCRIBE` so the hook is in the ledger
  (the old raw `SUBSCRIBE_METHOD` was invisible to the inventory) · the log no longer states
  DynamicTrainRoutes as fact.
- **Origin is not ours.** The upstream report is the real fix and now has a number behind it.
- **Verified:** build-only — `Result: Succeeded`.

---

## 2026-08-09 15:05 — CODE — P1.3 part 2: the SaveSettings interceptor. Phase 1's last piece

- **What:** `FFPMSaveSettingsInterceptor` — two hooks on `UFGGameUserSettings::SaveSettings`
  (`FGGameUserSettings.h:115`). **Before:** release every hold on a capturable cvar so the game
  serialises the *player's* values. **After:** re-apply them. Plus `FPMCVarWriter::GetHolds()`, a
  flattened snapshot so the guard can suspend and resume through the ordinary public Hold/Release path
  rather than reaching into the ledger.
- **Why it is mandatory — measured, not argued.** The read-only `GetValueToSave` probe on the live
  0.5.7 boot: **28 cvar-backed settings would be written by a save right now, 16 of them sitting at
  exactly their default.** `sg.TextureQuality` at `3` with default `3` would still be persisted. So the
  "different from the default and marked as dirty" promise in `FGUserSettingApplyType.h:101-102` is
  false in practice and the AC4 disassembly reading is correct: no dirty gate protects us.
- **Release, never restore-a-remembered-value.** Release is the engine's tagged-history `Unset`, so the
  cvar falls back to the player's own layer. Remembering a prior value would be capture-and-restore —
  the ratchet R33 removed baseline capture to kill, because a captured baseline can be our own write.
- **The three §2.3.6 invariants, all present:** arm-time self-test · **permanent, latching** fail-safe
  (never self-heals; the writer refuses every mapped write afterwards) · refuse-to-arm-while-held.
  `IsHealthy()` fails CLOSED before arming, after a failure, and mid-suspension.
- **The self-test's reach is stated in its own log line.** It proves both hooks installed and the
  user-setting map answers. It does **not** call `SaveSettings` — doing so would write Ant's real
  `GameUserSettings.ini`, which is the harm being guarded. End-to-end needs a boot with a real save.
- **Clause 6 is NOT lifted.** Shipping the interceptor is the design's condition for lifting it, but the
  lift is a separate deliberate change: P1.5 Leg B (does the menu's APPLY button write at 0x08 and
  outrank our 0x07?) is unanswered. Startup applies at `GameSetting` 0x02, measured on 0.5.6 — the
  apply-button path is a different one.
- **Files:** `Public/Core/FPMSaveSettingsInterceptor.h` (new), `Private/Core/FPMSaveSettingsInterceptor.cpp`
  (new), `Public/Core/FPMCVarWriter.h`, `Private/Core/FPMCVarWriter.cpp`, `Public/Core/FPMDiag.h`,
  `Private/Core/FPMDiag.cpp` (new `FPM.Diag.SaveGuard` channel), `Private/FicsitsPerformanceManager.cpp`.
- **Verified:** build-only — `Result: Succeeded`. NOT boot-tested. A compile error on the way in was
  informative and is recorded in the source: the `_AFTER` handler takes no `Scope`, because an
  after-hook cannot cancel or override a call that already ran.

---

## 2026-08-09 14:45 — VERSION — 0.5.6 → 0.5.7

- **What:** `0.5.7`; pin generated. **PATCH** — a read-only diagnostic plus a log-timing repair. With
  the console closed, nothing to notice.
- **Contents:** the `GetValueToSave` save probe · the map's own read moved off `OnPostEngineInit`, which
  the 0.5.6 boot proved fires too early.
- **⚠ SERVER STILL 0.5.5.** Unchanged from 0.5.6: client-only deploy, so a DatHost join is refused.

---

## 2026-08-09 14:42 — CODE — ask the settings system what it would actually SAVE, and write nothing to find out

- **What:** a new read-only section in `FPM.D0` that polls `GetValueToSave()` on every cvar-backed
  setting and reports which ones a save would persist right now, with applied and default values.
- **Why it settles a real contradiction:** `FPMCVarWriter.h:30-33` and design §2.3.6 state, from
  disassembly (AC4), that `FGGameUserSettings` serialises every `mUserSettings` entry on every save
  **with no dirty gate**. The engine-side API says the opposite in its own words
  (`FGUserSettingApplyType.h:101-102`): *"Returns a non empty FVariant if we have a value to actually
  save i.e the value is different from the default value **and marked as dirty**"*. Both cannot be
  true, and which one is decides how the SaveSettings interceptor must be built.
- **Read-only by design, and that is the point.** The obvious experiment — hold a real US_*-backed cvar,
  force a save, see whether it stuck — deliberately risks writing into Ant's own settings, which is
  exactly what clause 6 exists to prevent. `GetValueToSave()` is `const` and takes no arguments, so the
  whole question can be asked without touching anything.
- **How to read it:** all empty ⇒ a dirty gate exists and is closed, and the interceptor's job may
  shrink to *never mark dirty*. Some non-empty ⇒ those are settings Ant genuinely changed. All
  non-empty ⇒ the no-dirty-gate reading is right and restore-before-serialise is mandatory.
  It is stated in the output that this does **not** yet prove our own cvar write cannot dirty one.
- **Files:** `Private/Core/FPMCVarProbe.cpp`. **Verified:** build-only — `Result: Succeeded`.

---

## 2026-08-09 14:26 — VERSION — 0.5.5 → 0.5.6

- **What:** `0.5.6`; pin generated. **PATCH** — everything in it is a diagnostic. By the player-notice
  test: with the console closed, Ant sees no difference. The one behaviour change, clause 6 refusing ~43
  more cvars, is invisible because clause 6 already refuses the whole set and FPM drives no renderer
  cvars yet.
- **Contents:** the derived US_*-backed table (66 cvars, generated from the assets) · `FPMUserSettingMap`
  with the runtime enumeration · `FPM.D0`'s 188-false-positive cross-check fix · the map's automatic
  post-engine-init report · `FPM.D0.Auto`, which runs the D0 read by itself once per boot.
- **Why it ships now:** the deployed build is `0.5.5` and predates all of it, so the boot that answers
  P1.3's gate cannot be taken against what is currently installed.
- **⚠ SERVER PARITY NOT DONE.** Only the CLIENT is being deployed. `RequiredOnRemote: true`, so a
  `0.5.6` client will be refused by the `0.5.5` DatHost server on join. Deploying the server needs it
  STOPPED, which affects SunFry — Ant's call, not one to make while she is asleep.

---

## 2026-08-09 14:20 — CODE — the user-setting map reports itself every boot, because a console command cannot be delivered from outside

- **What:** `FPMUserSettingMap::Init()`, bound to `FCoreDelegates::OnPostEngineInit` from `StartupModule`.
  Every boot now logs which cvars the game's own settings save would capture — **at the main menu, with
  no save loaded and nothing typed.**
- **Why, measured today rather than assumed:** a console command cannot be delivered to this game from
  outside. UE strips `-ExecCmds` in Shipping; SML reimplements it
  (`SatisfactoryModLoader.cpp:218-227`, queuing deferred commands) — but Steam replaces the command
  line with its own launch options. Both a direct launch and `steam://run/526870//-ExecCmds=...` came up
  as `-NO_EOS_OVERLAY -useallavailablecores`. A diagnostic that must be TYPED costs one of Ant's boots.
- **Post-engine-init, not `StartupModule`:** the module loads *during* engine init, so `GEngine` is not
  usable yet and the read would fail on every boot, leaving us silently on the vanilla tables — which in
  a log is indistinguishable from "there were no settings".
- The world-load refresh still runs and still matters: it is the one that sees **mod-registered**
  settings, once their game features have activated.
- **Files:** `Public/Core/FPMUserSettingMap.h`, `Private/Core/FPMUserSettingMap.cpp`,
  `Private/FicsitsPerformanceManager.cpp`. No version bump — ships with P1.3.
- **Verified:** build-only — `Result: Succeeded`. NOT boot-tested: the deployed build is 0.5.5 and
  predates this.

---

## 2026-08-09 13:42 — CODE — P1.3 part 1: the user-setting map, read from the running game; and D0's cross-check stops crying wolf 188 times

- **What:** new `FPMUserSettingMap` (`Public/Core/FPMUserSettingMap.h`,
  `Private/Core/FPMUserSettingMap.cpp`) owns clause 6's question — *"would the game's settings save
  capture this cvar?"* — for all three callers (writer, residue sentinel, `FPM.D0`). It unions a
  **runtime enumeration** of `UFGGameUserSettings::GetAllUserSettingsMap()` with the two compiled
  tables. `FPMCVarWriter::IsUserSettingBacked` is now a forwarder; both tables moved out of the writer.
- **Why runtime is the primary and not a nicety:** the compiled tables are vanilla-only *by
  construction* — they come from an export of the base game. The settings FPM most needs to see are
  mod-registered: the LightSettings mod's levers are mod-side SessionSettings assets that no export of
  FactoryGame can contain. Only the running process knows the full set.
- **⚠ The predicate that makes it correct, and that `FPM.D0` had wrong.** The map's keys are SETTING
  IDs, not cvar names. A setting owns a cvar only when `UseCVar` is true, and then the cvar's name is
  its `StrId` (`FGUserSetting.h:183-189`). D0 treated every key as a cvar name — so with 188 of 254
  vanilla settings driving no cvar at all, it would have printed **188 `CLAUSE 6 BLIND SPOT` lines that
  are not blind spots**. A diagnostic that cries wolf 188 times hides the one real finding in its own
  noise. It now resolves each setting, filters on `ShouldUseCVar()`, and de-duplicates by `StrId`
  (two settings may drive one cvar — `FGOptionInterfaceImpl.h:30-33`).
- **Fails safe in the honest direction:** an empty map is REFUSED rather than cached (empty means
  "asked too early", not "this game has no settings"); `Refresh()` no-ops when there is no settings
  object; a `false` answer while still on tables-only logs **once** that it means "not in the vanilla
  snapshot", not "safe to write".
- **Re-reads, deliberately.** Refreshed at `CONSTRUCTION` **before** `NotifyWorldLoad`, so a fix that
  holds a cvar in its own load handler is judged against this world's picture, not the previous one.
  Mods register settings as their game features activate, so a set captured once at the earliest
  opportunity is a vanilla map wearing a runtime label.
- **Files:** `Public/Core/FPMUserSettingMap.h` (new), `Private/Core/FPMUserSettingMap.cpp` (new),
  `Private/Core/FPMCVarWriter.cpp`, `Private/Core/FPMCVarProbe.cpp`,
  `Private/Module/RootGameWorld_FicsitsPerformanceManager.cpp`. No version bump — ships with P1.3.
- **Revert:** point `IsUserSettingBacked` back at the tables and drop the `Refresh()` call sites.
- **Verified:** build-only — `Result: Succeeded`. NOT boot-tested: the runtime enumeration has never
  run, and until it does, `Source()` reports `TablesOnly` by design.

---

## 2026-08-09 13:30 — CODE — clause 6's denylist re-derived from the assets; it was missing 56 cvars, including the one the design records as having leaked

- **What:** `IsUserSettingBacked` now checks a GENERATED table of the 66 vanilla cvar-backed user
  settings (`Private/Core/FPMUserSettingTable.g.h`) **in union with** the old hand-guessed list, which
  is retained and renamed `GFPMWriterUSLegacyGuesses`.
- **Why:** the old list guessed a cvar name from an ASSET name. Read from the assets instead — a
  setting's cvar name is its `StrId` (`FGUserSetting.h:183-189`, "manage and if needed create a cvar
  for this setting based on StrId"; confirmed on `US_MaxFPS`, whose StrId is `t.MaxFPS`) — the picture
  changes completely:
  - **56 real US_*-backed cvars were absent** from the shipped list. 13 are `sg.*` and already refused
    by clause 5, so ~43 were genuinely unguarded — among them **`r.ContactShadows`, the very cvar
    §2.3.6 records as having caused a LIVE residue leak on 2026-08-02**. Latent, not active: FPM drives
    no renderer cvars yet.
  - **`r.Gamma` was refused while `r.TonemapperGamma` — the one `US_Gamma` actually drives — was not.**
    It guarded a name the game does not use and left open the one it does.
  - The famous **"242 of 272 UNMAPPED"** was an artefact of how that file was built, not a knowledge
    gap: of its 272 rows, 184 are settings that drive no cvar at all, 4 are names truncated at a hyphen
    (`US_HierarchicalZ` for `US_HierarchicalZ-BufferOcclusion`), and 19 match no asset in the game
    (`US_34z`, `US_Jx`, `US_dT`, …). Reading StrId yields 66 cvar-backed and **zero** unmapped.
- **Union, not replacement.** Every legacy entry the derived table lacks is an asset with no cvar or a
  cvar no asset claims, so all 23 look safe to drop — but "looks safe on this evidence" is the sentence
  that preceded the 242 mistake, and the costs are asymmetric: a false refusal is a log line, a false
  permission is a permanent change to the player's own settings.
- **Neither table is the primary, and both are VANILLA-ONLY.** Confirmed this session: the LightSettings
  mod (`"Let there be Light"`, folder `LightSettings`) exposes its levers as mod-side **SessionSettings**
  assets — `LS_SS_AttenuationRadius` (StrId `LightSettings.AttenuationRadius`, default `5000`, the one
  Ant dropped to 1000), plus SourceRadius / SoftSourceRadius / LightFalloff / Inner+OuterConeAngle. None
  carries `UseCVar`, so **none is a cvar at all** and no export of the base game could ever contain
  them. `GetAllUserSettingsMap()` at runtime stays the primary; these tables are the fallback.
- **Files:** `Private/Core/FPMCVarWriter.cpp`, `Private/Core/FPMUserSettingTable.g.h` (new, generated),
  `40-TOOLS/satisfactory/extract_user_settings.ps1` (new, brain repo),
  `40-TOOLS/satisfactory/us_settings.tsv` (new, brain repo). No version bump — ships with P1.3.
- **Revert:** restore `GFPMWriterUSLegacyGuesses` to the name `GFPMWriterUSDenylist`, drop the include
  and the derived loop. Undo if the derived table is ever shown to refuse a cvar FPM must write — the
  refusal log names the cvar and its asset, so that case arrives identified.
- **Verified:** build-only — `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`. NOT boot-tested: no behaviour change is observable while clause 6 refuses the
  whole set anyway.

---

## 2026-08-09 12:40 — VERSION — 0.5.4 → 0.5.5

- **What:** `0.5.5`; pin generated. PATCH — `FPM.D0` is a console command; a player would not notice.

---

## 2026-08-09 12:38 — CODE — `FPM.D0`: Phase 2's console half in one command

Ant's standing rule: *"automate as much as possible by default. i dont like running around throwing
commands around."*

- **`FPM.D0`** runs the whole D0-client console read: the **`GetAllUserSettingsMap` enumeration**
  (`UFGGameUserSettings`, `FGGameUserSettings.h:330`) — **which is what P1.3 is gated on** — plus the
  GI / contact-shadow / distance-field / MegaLights cvar reads with their full priority-layer stacks.
- **It cross-checks every enumerated setting against clause 6** and flags any the game persists that
  `IsUserSettingBacked` does not protect. That check replaces a hand-maintained list — which is wrong
  the moment Coffee Stain adds a setting, and wrong SILENTLY — with the game's own answer.
- **It states what it could NOT cover**: the ten-dismantle baseline, pop-in / connector watches, the
  hypertube ride. An automated report that quietly omits the manual half reads as a complete
  discovery pass, and the next session plans against it.
- Unreachable settings object prints *"we did not look"*, never *"no settings"*.
- **Files:** `Private/Core/FPMCVarProbe.cpp`. **Verified:** build-only, `Result: Succeeded`.

---
## 2026-08-09 11:45 — VERSION — 0.5.3 → 0.5.4

- **What:** `0.5.4`; pin generated. PATCH — `FPM.Bisect` is a console command; a player would not notice.
- **Verified:** predicate after tagging. Boot-tested: **NOT YET.**

---

## 2026-08-09 11:42 — CODE — `FPM.Bisect`, and two instruments that were wrong about themselves

### `FPM.Bisect <sg.Group> <badLevel> <goodLevel>`

Ant: *"no more command spam. lets build the mod so it can do this itself. this is what the bench is
for anyways."* One command. It applies each level, samples wall-clock frame time over a fixed window,
diffs the LIVE cvars to build its candidate list, then sets each candidate to its good value **alone**
while the rest stay bad — and prints a ranked table of what each one recovers.

Everything about it is shaped by a failure from the same afternoon:

- **Live cvars, never an ini.** We bisected against the ENGINE's `BaseScalability.ini` for hours;
  three of six candidates did not exist in Satisfactory's table at all.
- **Wall clock, not `FApp::GetDeltaTime()`** — that is smoothed and clamped (`Engine.h:1552`) and
  would flatten the very differences being measured. Same finding the hitch meter rests on.
- **Console priority, deliberately, and `Unset` between candidates** — console is the only layer that
  beats the group's own value, and console writes STICK, so without the unset the candidates would
  accumulate and every row after the first would be a lie.
- **It reports what it cannot account for.** If the best single cvar explains under half the gap it
  says so and tells you not to pick a ladder lever off the table. **On today's evidence that is the
  likely result** — see below.
- A gap under 0.5 ms prints "nothing to attribute" rather than ranking jitter into a confident table.

### ★ WHY NO SINGLE CVAR WAS EVER GOING TO WORK — Ant's correction

I had just written that the cost was the sun and *"the lamps were never the story"*. Ant: **"wait, but
the lamps DOES make it worse. those are also light sources."** She is right, and the correction gives
the model that actually fits the day's data:

| scene | the light doing the work | cvar family |
|---|---|---|
| night lamp corridor (39 → 114 with the group) | ~40 local point lights | cube maps — `r.Shadow.MaxResolution`, `RadiusThreshold` |
| daylit pond, no buildings (53 → 77 at night) | one directional sun | cascades — `r.Shadow.DistanceScale`, `MaxCSMResolution` |

**Two cvar families, one shared bill.** `sg.ShadowQuality` moves both at once, which is why the GROUP
recovers 65 fps while every individual cvar tested could only ever govern the half that was not
dominant where she happened to be standing. Each partial result was read as "excluded" when it may
have meant "this one owns the other half".

### The `FPM.Prove` step-4 FAIL was the TEST's bug, not the writer's

First real run printed `[FAIL] 4 release restores value AND SetBy  2048 (Scalability) -> 888`. The
writer was correct: step 2's `Set(..., ECVF_SetByScalability)` does not stack on the game's value at
that priority, it **replaces** the slot — so release fell back to the Scalability layer exactly as
designed, and the layer was no longer 2048 because our own test had overwritten it. Step 2 now writes
the original value back before step 4 runs. **A test that reports FAIL on a working system is worse
than no test** — it would have sent us hunting a release bug in the path the zero-residue promise
rests on. Steps 1–3 passed and are the law question: `PluginHighPriority` took, survived a Scalability
write, and the console still beat us.

### The 48 UNACCOUNTED, found twice

0.5.1 printed `** 48 UNACCOUNTED **`. I blamed `HandleClass`'s `!CDO` guard and added a counter there.
0.5.3 printed `0 no usable CDO | ** 48 UNACCOUNTED **` — same 48, my explanation disproved by the
counter I added to confirm it. The skip is one line ABOVE `HandleClass`: abstract, deprecated and
superseded classes are `continue`d out of the sweep loop while still counting toward the denominator.
Now `%d not instantiable`. **The arithmetic was right both times; my guess at where was wrong, and
only a counter that could disprove it revealed that.**

- **Files:** `Private/Core/FPMCVarProbe.cpp`, `Private/Fixes/Interop/FPMRainOcclusionFix.cpp`.
- **Verified:** build-only, `Result: Succeeded`. Not boot-tested.

---

## 2026-08-09 11:35 — VERSION — 0.5.2 → 0.5.3

- **What:** `0.5.3`; pin generated. PATCH by the player-notice test — `FPM.Prove` is a console command.
- **Verified:** predicate after tagging. Boot-tested: **NOT YET.**

---

## 2026-08-09 11:30 — CODE — `FPM.Prove` runs the whole P1.5 protocol, and the ledger stops lying

**Ant, mid-way through running the 0x07 proof by hand:** *"okey this is too many commands and its
confusing. we need to make the mod do this."* then *"no more command spam. lets build the mod so it
can do this itself. this is what the bench is for anyways."*

- **`FPM.Prove`** — one command, no follow-ups. It runs P1.5's five questions and prints PASS/FAIL per
  step:
  1. does a tagged FPM write take?
  2. does it survive a **Scalability**-priority write? *(the whole ladder rests on yes)*
  3. does the **CONSOLE** still beat FPM? *(must be yes — otherwise we have taken the operator's
     override away, which is worse than any performance win)*
  4. does release restore the value **AND the SetBy**? *(the zero-residue promise, both axes)*
  5. does it survive the vanilla options-menu apply? *(the path a player actually uses)*
- **Step 5 needs a human to click Apply, so the probe ARMS A WATCHER rather than becoming a second
  command.** It holds the cvar, polls at 2 Hz for 180 s, and reports by itself the moment the menu
  moves it — naming the SetBy that won. **A timeout prints `-- inconclusive`, never PASS:** "nobody
  applied anything" and "the hold survived an apply" are different facts and must not share a line.
- **It always cleans up.** Every exit path releases the hold and `Unset`s the probe's own console
  write — by priority, not by setting a value back, because a lower Set appends another history layer
  instead of removing ours. *A proof that leaves residue has disproved the thing it set out to show.*

### ⚠ AND THE LEDGER COULD NOT REPORT A LOST HOLD — caught by Ant, mid-protocol

`FPM.Changes` printed `H.Value`: what we **asked** to hold. So a hold that had been BEATEN by a
higher-priority writer printed a line **identical** to one still in force. During the 0x07 proof that
is the entire question, and the command answering it could not distinguish the two outcomes. Ant read
`= 1024` after a menu apply and said *"changed a setting but it didnt take according to this"* — the
output genuinely could not tell her which had happened.

**A ledger cannot verify itself: it is a record of intent, and intent is not state.** Every row now
reads the live variable back and prints `HOLD IN FORCE` or
`** OVERRIDDEN - our value is NOT what the game is using **`. A divergence means either something
outranked us (a real finding) or our release path failed and we are tracking a hold that no longer
exists (residue).

- **Files:** `Private/Core/FPMCVarProbe.cpp`, `Private/Core/FPMCVarWriter.cpp`.
- **Revert:** both are additive to diagnostics; no fix behaviour changes.
- **Verified:** build-only, `Result: Succeeded`. Not boot-tested.

---

## 2026-08-09 11:15 — VERSION — 0.5.1 → 0.5.2

- **What:** `VersionName` / `SemVersion` → `0.5.2`; pin generated by `stamp_version.py --stamp`.
- **Why PATCH:** everything in it is a **debug console surface**. Six new commands, and a player with
  the console closed would not notice one of them. New is not the same as user-visible.
- **⚠ CUT AS `0.6.0` FIRST, AND THAT WAS THE SECOND TIME IN ONE DAY.** I applied the rule I had
  written that morning — *"could you describe the release without mentioning the new command?"* — and
  it said MINOR, because four new commands genuinely are the release. Ant: *"again, the big bump is
  kinda too much for a small edit."* She is right, and the rule was the problem, not the arithmetic:
  it passes anything sufficiently novel. **The test is now "would a player notice while playing, with
  the console closed?"** and it lives in the versioning section above rather than only in this entry.
  Tag `0.6.0` deleted locally and on `origin`; nothing had been booted on it.
- **Verified:** version predicate after tagging. Boot-tested: **NOT YET.**

---

## 2026-08-09 11:10 — CODE — the cvar probe, and the commands that make P1.5 runnable at all

- **`FPM.CVars [prefix ...]`** — every matching cvar with the priority layer that currently owns it.
- **`FPM.CVarHistory <name>`** — the FULL layer stack (Constructor / SystemSettingsIni / Scalability /
  Console) via `IConsoleVariable::LogHistory`, which is public at `IConsoleManager.h:658`.
- **`FPM.CVarSnap <A|B> [prefix ...]` + `FPM.CVarDiff`** — snapshot, then print exactly what changed
  between two states, with the owning layer beside each value.
- **`FPM.Hold <cvar> <value>` / `FPM.Release <cvar>`** — hold through the writer, at FPM's priority.
- **Every one of these takes an `FOutputDevice`,** and so do `FPM.Changes` and `FPM.Off` now.

**WHY, and it is a measured failure rather than a feature idea.** Ant lost ~65 fps in her base to
`sg.ShadowQuality 3` and an afternoon of manual bisecting could not name the cvar. Three separate
defects in the METHOD, each one now closed by something above:

1. **We were reading the wrong table.** Every candidate list came from the ENGINE's
   `BaseScalability.ini`. Satisfactory ships its own, there is no loose copy in the install, and the
   game's real values were only ever visible from inside the running process. Proven the hard way:
   her console showed `Scalability: 512` for `r.Shadow.MaxResolution` where the engine ini says 1024.
   *(Resolved the same day — the real table was in her FModel export all along, at
   `20-SOURCES/satisfactory/fmodel-exports/FactoryGame/Config/DefaultScalability.ini`. The probe still
   earns its place: it verifies what is ACTUALLY APPLIED, which a file cannot.)*
2. **`FPM.Changes` printed where she was not looking.** It used `UE_LOG`, so it wrote to
   FactoryGame.log while she watched an empty console and reasonably concluded it was broken. **A
   diagnostic that answers somewhere the operator is not looking is worth the same as one that does
   not answer.**
3. **CONSOLE WRITES OUTRANK SCALABILITY AND STICK FOR THE SESSION.** Once a cvar is typed, `sg.*`
   cannot move it again. "Re-apply the group to reset" — my own instruction — silently reset nothing
   already typed, contaminating hours of A/B. `FPM.CVarDiff` prints the owning layer beside every
   value, so that contamination is visible instead of invisible.

- **★ P1.5 COULD NOT BE RUN, and nobody had noticed.** Design R2 §9's last Phase 1 increment is the
  0x07 proof boot, and it **blocks a law change** — the recorded law keeps prescribing `SetByCode`
  until it lands. Writing the protocol out for Ant revealed that **nothing in the shipped build makes
  the writer hold a cvar on demand**: the boot self-test holds and releases within one frame, and no
  fix writes anything yet. So every instruction of the form *"hold a cvar, then change a setting"* was
  unexecutable. The protocol had been reviewed several times; the gap was in the BUILD.
- **Clause 6 still applies to `FPM.Hold`,** so it covers P1.5 **Leg A only**. Leg B deliberately
  targets `t.MaxFPS` — US_*-backed — to contest 0x08 `SetByGameOverride`. Crossing that boundary is
  Ant's ruling, not a decision to bury inside a console command.
- **Files:** `Private/Core/FPMCVarProbe.cpp` (new), `Private/Core/FPMCVarWriter.cpp`,
  `Public/Core/FPMCVarWriter.h`.
- **Revert:** the probe is one self-contained file and sets nothing — deleting it removes four
  commands and changes no behaviour.
- **Verified:** build-only, `Result: Succeeded`. Not boot-tested.

---

## 2026-08-09 10:45 — VERSION — 0.5.0 → 0.5.1

- **What:** `VersionName` / `SemVersion` → `0.5.1`; pin generated by `stamp_version.py --stamp`.
- **Why PATCH:** every change in it is a **repair to an existing surface** — the overlay printed
  forever, truncated the session totals off the right edge, and covered her FPS readout; the rain
  sweep could not report a non-zero after the first world load. **No new capability: the mod does
  nothing today it did not do at 0.5.0.**
- **⚠ THIS WAS FIRST CUT AS `0.6.0` AND THAT WAS WRONG.** The reasoning was that `FPM.Diag.Clear` and
  `FPM.Diag.OverlayTop` are "new surface", which the table puts at MINOR. Ant: *"why the big bump for
  a small patch? you should be better at bumping the proper way."* She is right, and the rule the
  table means is now written down so it is not re-derived wrongly: **"new surface" means the MOD does
  something new. A cvar or command that exists only to make a REPAIR configurable is part of the
  repair, not a capability.** By that test `FPM.Diag.Clear` is plumbing for "the overlay printed
  forever" and `FPM.Diag.OverlayTop` is plumbing for "it covered her HUD" — both PATCH.
  Corrected before anything was deployed; the `0.6.0` tag was deleted locally and on `origin`.
- **Verified:** version predicate after tagging. Boot-tested: **NOT YET.**

---

## 2026-08-09 10:40 — CODE — the overlay stops printing forever, and stops truncating measurements

All four came from Ant watching the 0.5.0 overlay during a live session, in her words.

- **`FPM.Diag.Clear`** — *"i also need a way to reset this window, since it just prints forever."*
  Screen only; the log is untouched and still holds every line, so clearing can never destroy evidence.
- **THE REASON SHE NEEDED IT, which matters more than the reset.** The hitch summary is a **gauge** —
  it always has a current reading — and appending one every 60 s filled all 18 panel rows with hitch
  history inside twenty minutes, scrolling the startup and rain-sweep lines off the top. Clearing
  would have bought eighteen more minutes. `PostSticky(Category, Key, Line)` now rewrites one row per
  key in place, and `LogSummary`'s own `Reason` is the key, so `running` keeps one row while
  `world load` keeps its own rather than being overwritten by the next rolling window. No new state.
- **`FPM.Diag.OverlayTop`** (default 44 px) — *"had to turn off debug ui since it blocked the fps
  number."* Her hardware monitor owns the top strip and the panel was anchored at y=16. A cvar rather
  than a number I pick, for the same reason the hotkey is one: the right value depends on HER overlay
  and resolution, and a hard-coded guess that collides is unfixable without a build.
- **Wrapping** — *"it also cuts off to the right side of the screen. better if it just went down a
  row instead."* The hitch line is long **by design** (every figure carries its denominator), so it
  overran 1440p and the part being lost was the **session totals at the END** — the worst possible
  place to truncate a measurement. Wraps at the viewport edge via a lambda attribute, so a resolution
  or DPI change re-wraps without rebuilding the widget.
- **⚠ REVIEW FINDING ON MY OWN CHANGE, caught by the version-bump pass before it shipped.** Sticky rows
  were evicted by the same `RemoveAt(0)` as events, so eighteen events piling up behind the hitch
  summary would have made it silently vanish and reappear at the bottom a minute later — the exact
  "scrolls the useful line away" behaviour this change exists to end. Eviction now skips gauge rows.
  **Fixing the symptom while keeping the disease is the failure mode; the pass exists to catch that.**
- **Files:** `Public/Core/FPMOverlay.h`, `Private/Core/FPMOverlay.cpp`, `Private/Core/FPMHitchMeter.cpp`.
- **Revert:** each is independent. `FPM.Diag.OverlayTop 16` restores the old position without a build.
- **Verified:** build-only, `Result: Succeeded`. Not boot-tested.

---

## 2026-08-09 10:40 — CODE — the rain sweep could not report a non-zero after the first world load

- **What:** two silent skips in `HandleClass` are now counted — `already settled` (handled by an
  earlier sweep this process) and `needed no repair` (vanilla already gave it a usable box) — and the
  summary prints `⚠ N UNACCOUNTED` if the six buckets stop summing to the examined count.
- **Why, from Ant's second world load on 0.5.0:**
  `cache HIT | 3679 classes examined | 0 from cache, 0 instance-data, 0 components, 0 none`.
  Every bucket zero against a denominator of 3679. **Nothing was broken** — CDOs live for the whole
  process, so a class settled by the first sweep genuinely needs no work on the second, and
  `GHandledClasses` correctly short-circuits it. But both early returns incremented nothing, so *"we
  already did this"* and *"we did nothing"* printed identically.
- **This is the DEAD INSTRUMENT smell in its subtler form.** The line is not incapable of a non-zero in
  general — it was informative on the first sweep. It is incapable of one on **every sweep after the
  first**, which is every sweep in a long session. A previous fix made the counters per-sweep to stop
  them re-printing stale totals; that fix is what exposed this one.
- **⚠ AND THE SAME BUG, OPPOSITE DIRECTION, caught on the bump review:** I added `off-thread skips` to
  the line while it is the one counter deliberately NOT reset per sweep (the hooks fire between
  sweeps, so resetting would discard the interesting ones). An unlabelled cumulative figure beside six
  per-sweep ones is exactly the defect the reset comment records. It now says **"this session"**.
- **Files:** `Private/Fixes/Interop/FPMRainOcclusionFix.cpp`.
- **Revert:** cosmetic only — no behaviour outside the summary string changed.
- **Verified:** build-only, `Result: Succeeded`. Not boot-tested.

---

## 2026-08-09 09:48 — VERSION — 0.4.1 → 0.5.0

- **What:** `VersionName` / `SemVersion` → `0.5.0`; `RemoteVersionRange` → `=0.5.0`, **generated** by
  `stamp_version.py --stamp`, never typed. Descriptor `Description` gained the F8 keybind and the
  uninstall-cleanliness claim, both of which are user-visible and were missing from the ficsit.app text.
- **Why MINOR and not PATCH:** the table above puts "new capability, new surface" at MINOR, and 0.5.0 adds
  both. Phase 1 shipped the CVar writer, the residue sentinel, the origin/channel contract, and an overlay
  keybind; the mod now registers **26 distinct `FPM.*` console surfaces** where 0.4.1 had far fewer. No
  existing behaviour changed incompatibly, so this is not MAJOR.
- **Release notes for this version** = the six `CODE` entries below, back to the `0.4.0 → 0.4.1` line.
- **Files:** `FicsitsPerformanceManager.uplugin`. **No source file carries a version literal** — the runtime
  banner reads the loaded descriptor, so there is exactly one place to bump and the log cannot disagree
  with the file.
- **Revert:** re-run `stamp_version.py --stamp` after restoring `SemVersion`, and delete the `0.5.0` tag.
  Undo if the boot below reveals a Phase 1 fault serious enough to unship.
- **Verified:** version predicate `0.5.0 == 0.5.0 == =0.5.0` after tagging. Packaged + deployed + booted:
  see the boot line appended to this entry. **NOT-YET at the time of writing.**

---

## 2026-08-09 09:35 — CODE — third pass: the residue check's path prefix needed a trailing separator

- **What:** `FPMBoxCache`'s plugin-dir prefix now ends in a separator before `StartsWith`.
- **Why:** without it the comparison is a plain string prefix, so a sibling directory named
  `FicsitsPerformanceManagerAnything` would be classified **"inside the plugin, not residue"**. Latent —
  no such directory exists — but a false NOT-RESIDUE verdict is the single error this checker must never
  make, and it would be completely silent.
- **Checked and found clean in the same pass**, recorded so it is not re-derived: `CleanUpLegacyResidue`
  can only ever target one fully-determined path (engine Saved dir + two literals, no pattern), and
  `DeleteDirectory` with `Tree=false` makes the OS refuse a non-empty directory regardless of whether our
  own emptiness check is right — the guard is the belt, `Tree=false` is the braces. `Audit`'s two counters
  are genuinely separate: `WouldRemain` for undeclared cvars, `FileResidue` for the declared file, only the
  former returned, so the drill cannot break on a read-only install.
- **Files:** `Private/Core/FPMBoxCache.cpp`.
- **Revert:** drop the separator. Do not — the failure is silent and in the safety path.
- **Verified:** build-only, `Result: Succeeded`.

---

## 2026-08-09 09:28 — CODE — second pass over the fix round itself: four more, one of which broke the drill

- **What:** reviewing the FIX ROUND with the same scrutiny as the code it fixed. Four defects, all mine,
  all introduced by the previous entry.
  1. **Self-contradiction.** The file branch logged the words *"the declared fallback"* while the exception
     table held zero entries — the audit simultaneously claimed the cache was sanctioned and counted it as
     unsanctioned residue. **Prose is not a declaration.** It is now an actual declared exception, which is
     what Ant's ruling said.
  2. **Wrong remedy.** The failure verdict printed *"find the write path that bypassed clause 6"* for any
     non-zero result — so a FILE would have sent the reader hunting a cvar bug that does not exist. Cvar
     residue and file residue now carry separate verdicts and separate remedies. **A wrong remedy in a
     diagnostic costs more than no remedy.**
  3. **The drill would have failed permanently on any read-only install.** It required `Audit() == 0`, and
     a fallback cache made `Audit()` return 1 forever — so it would report *"FPM is capable of leaving
     residue"* while blaming a release path that worked perfectly. `Audit()` now returns only the
     undeclared **cvar** count, with a comment saying why widening it breaks the drill.
  4. **Silence on the no-file case.** Neither branch ran when no cache existed, so *"we looked and there is
     none"* and *"nobody checked files"* produced identical output — the exact equivalence that let this
     input be missing in the first place. It now says so.
- **Tooling ladder closed for this work** (bank → catalogue → online): there is no off-the-shelf residue
  auditor for UE mods; the ecosystem convention is that the *user* deletes `Saved/` and `Intermediate/` by
  hand. Building it was correct, and keeping the cache inside the plugin is stricter than the norm.
- **Files:** `Private/Core/FPMResidueSentinel.cpp`, `Private/Core/FPMBoxCache.cpp`.
- **Revert:** each of the four is independent; revert individually. Finding 3 is the one that matters.
- **Verified:** build-only, `Result: Succeeded`.

---

## 2026-08-09 09:22 — CODE — review pass on P1: three defects, and the residue the auditor could not see

- **Reviewed inline** after two subagents stalled with 0-byte transcripts. Every finding here was found by
  hand.
- **★ FOUND BY THE SPEC AXIS, and it is the big one.** FPM wrote a **120,681-byte** derived-box cache to
  `%LOCALAPPDATA%/FactoryGame/Saved/FicsitsPerformanceManager/DerivedBoxes.json` — **outside the plugin,
  surviving uninstall** — while `FPM.Residue` reported *"NOTHING would remain"*. The auditor inspected
  cvars only; **file residue was a category it could not see.** Sixth dead instrument in two days, in the
  file whose entire job is catching exactly this.
  - The cache now writes **inside the plugin** when that directory is writable — probed by an actual write,
    every boot, because permissions and read-only media lie to every cheaper check — falling back to
    `Saved/` with a loud declaration when it is not. Ant's ruling: *"would be good to keep it in the mod if
    possible, if its not we declare it an exception."* `Program Files` is writable on her Steam install but
    is not universally, and FPM ships publicly.
  - The sentinel gained **INPUT 4: files**, classifying inside-plugin vs outside and counting the latter.
  - A one-time cleanup deletes the exact legacy path at startup, and the directory only if that left it
    empty. Never a pattern, never recursive.
- **Q1 defect:** `Drill()` called `ReleaseAll`, dropping **every** hold in the process. Harmless today, but
  once the governor exists, running a diagnostic would silently tear down live levers. Now `ReleaseOwner`;
  `FPM.Off` still covers the all-or-nothing case.
- **Q2 defect:** `Hold()` captured `PriorValue` by reading the cvar, so on a **re-hold** it recorded our own
  previous value as the player's baseline. Release was never affected — the engine's history handles that —
  but `FPM.Changes` would have answered *"what has this mod changed?"* with a value the mod set. The first
  hold's prior now survives re-holds.
- **Q3, Q4: no finding.** Int cvars round-trip `GetString` cleanly; the observe-mode loop warns on a
  different owner and stays silent on reclaim.
- **The classifier now proves itself**, per Ant's *"dead instruments are not my preferred item to exist."*
  The `US_*` check can never fire normally, because clause 6 refuses the writes that would trip it —
  correct behaviour that leaves the detector unproven. So the classifier is **asserted at every audit**:
  `t.MaxFPS` must classify as backed, `FPM.SelfTest.Probe` must not. If either fails, the audit reports
  that it is **blind** rather than printing a clean sheet.
- **Files:** `Private/Core/FPMBoxCache.cpp`, `Public/Core/FPMBoxCache.h`,
  `Private/Core/FPMResidueSentinel.cpp`, `Private/Core/FPMCVarWriter.cpp`,
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert:** the cache relocation is the only player-visible one — reverting it re-creates the residue.
- **Verified:** build-only, `Result: Succeeded`.

---

## 2026-08-09 09:10 — CODE — P1.4: the residue sentinel, whose first draft could not fail

- **What:** `Core/FPMResidueSentinel.{h,cpp}`. `FPM.Residue` audits what would remain on the machine if
  the game saved settings now and FPM were deleted. `FPM.ResidueDrill` holds, audits, releases through
  the OFF switch, audits again, and checks the value **and** the SetBy came back.
- **Why the SetBy too:** a drill that only checked the value would pass while our 0x07 tag still sat on
  the variable, locking out every lower-priority writer. That residue is invisible to anyone reading the
  number.
- **⚠ WORTH RECORDING RATHER THAN QUIETLY FIXING.** The first version of `Audit()` set `WouldRemain = 0`
  and never iterated anything, because the ledger was private to the writer. It would have printed a
  confident "NOTHING would remain" for the rest of the mod's life regardless of what FPM held. **An
  auditor whose result is structurally constant is a decoration, not a weak auditor** — and it is the
  fifth instance of this defect class in two days, in the one file whose whole job is catching it. The
  writer now exposes `GetHeldCVars()` and `IsUserSettingBacked()` so the sentinel classifies real holds
  and shares ONE declaration of the rule rather than keeping a copy that can drift.
- **Stated in the code, not only here:** a leak from a PAST session is invisible by construction (no
  retroactive scanner is built, deliberately — one that guessed which of a player's settings were ours
  would be worse than the leak), and a P1 pass is near-trivial because almost nothing is registered yet.
  The drill becomes a real gate at P5.
- **The named-exception table is EMPTY, and that is a real state.** FPM2 writes no ini, no save, no
  registry. It is enumerated anyway so that the day something must persist, it arrives with a ruling, a
  boot-log mention and a row — not as a quiet `GConfig->SetString` inside a fix.
- **Files:** `Public/Core/FPMResidueSentinel.h`, `Private/Core/FPMResidueSentinel.cpp`,
  `Core/FPMCVarWriter.{h,cpp}` (two read-only accessors).
- **Verified:** build-only, `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 09:00 — CODE — P1.2: FPMCVarWriter, the single write path

- **What:** `Core/FPMCVarWriter.{h,cpp}` — design §2.3, clauses 1–5, 7, 8. `Hold` / `Release` /
  `ReleaseOwner` / `ReleaseAll`, an in-memory ledger, `FPM.Changes` (what we hold and what it was
  before), `FPM.Off` (the OFF switch), and a boot self-test on FPM's own probe cvar.
- **Clause 6 REFUSES the US_*-backed set entirely** until P1.3. That is correct behaviour, not a stub: a
  value FPM holds on a US_*-backed cvar at save time becomes the player's permanent setting, because
  `FGGameUserSettings` serialises every entry with **no dirty gate**. ⚠ The denylist is
  **known-incomplete and says so** — of 272 US_* assets, **242 are UNMAPPED** and name no cvar at all.
  Absence from the map is an absence claim, not safety; that is why the clause refuses the subset rather
  than filtering by the table, and why P1.3 reads the map at runtime.
- **Priority:** `ECVF_SetByPluginHighPriority` (0x07), verified at `IConsoleManager.h:143-171` — above
  scalability, game settings and device profiles; below commandline, SetByCode and console, so Ant's own
  console write still beats us.
- **★ RELEASE USES THE ENGINE'S MECHANISM, NOT A LOWER WRITE.** 0x07 is an ARRAY-typed priority; the
  engine's own comment calls it *"used with the History concept to restore cvars on plugin unload"*. So
  release is `Unset(priority, Tag)` (`:570`), which REMOVES our history entry. A lower-priority `Set`
  would APPEND to the array and leave our value in the stack forever — a leak that looks exactly like a
  working revert. `ReleaseAll` is the engine-native `UnsetAllConsoleVariablesWithTag` (`:1243`), one
  call, so the OFF switch cannot half-work.
- **The self-test runs every boot**, on FPM's own cvar, never a game cvar — because what it checks is an
  ENGINE behaviour a game update can change under us, and a release path that silently stopped working
  would look perfect right up until an uninstall left residue behind.
- **⚠ OPEN AND STATED:** `ECVF_SetByGameOverride` (0x08) sits ABOVE us and is documented as the slot for
  GameUserSettings fields. If the game's apply path uses it, our hold on that subset loses **silently**.
  P1.5's boot answers it; **no document may claim "0x07 wins" about the US_* subset until then.**
- **Files:** `Public/Core/FPMCVarWriter.h`, `Private/Core/FPMCVarWriter.cpp`,
  `Private/FicsitsPerformanceManager.cpp` (self-test at startup, `ReleaseAll` at shutdown).
- **Verified:** build-only, `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 08:50 — CODE — P1.1: a fix cannot compile without declaring its claim and its channel

- **What:** `IFPMFix` gains two PURE VIRTUALS — `OriginStatus()` (Ant's Q1 ruling) and `Channel()`
  (§3.1). Pure, not defaulted: a default lets a new fix silently inherit someone else's answer, and the
  entire value is that the author cannot avoid answering. Plus `FPM.Diag.Dump`, three new channels
  (`StaticBase`, `RpcGate`, `Rain`) for fixes that had diagnostics and no channel, and a runtime
  order-check in `FPM.Diag.List` against each cvar's registered name (`IConsoleManager.h:1104`).
- **The changelog rule this establishes:** `EFPMOriginStatus` has four values and **"fixed" may only
  appear beside `OriginNamed`.** Everything else is named as what it is. The log strings say the
  uncomfortable part out loud — "choke-point repair, CAUSE NOT NAMED" rather than a word that reads like
  a technique.
- **The honest retroactive classification of all nine fixes:** origin named — static-base,
  rain-occlusion, asset-residency. Guard — no-owner-rpc-gate (the cause is in another mod's sign
  dispatch; the upstream report IS the origin work). Choke-point repair — inventory-init,
  hologram-net. **Unknown cause — schematic-probe, clone-sensor, hitch-meter**, all three of which ARE
  their families' origin-naming instruments. **Six of nine do not rest on a named cause, and
  `FPM.Diag.Dump` prints that count** so it can be watched going down.
- **The "DELIBERATELY FOUR MEMBERS" freeze comment was rewritten in the same commit** as the additions,
  as the design requires — a comment contradicting the code it sits on is this project's own named
  defect.
- **GATE EXERCISED (LAW 16), not assumed:** removing `Channel()` from one fix produced
  `error C2259: cannot instantiate abstract class`, `Result: Failed`. Restored, rebuilt, `Succeeded`.
- **Files:** `Core/FPMFixContract.{h,cpp}`, `Core/FPMDiag.{h,cpp}`, all nine fix headers.
- **Verified:** build-only + the gate exercise above. **NOT boot-tested.**

## 2026-08-09 08:40 — CODE — overlay keybind, and the cache no longer invalidates on our own version

- **What:** `FPM.Diag.OverlayKey` (default **F8**, empty disables) toggles the debug overlay, via a Slate
  input pre-processor (`SlateApplication.h:1522`). It returns false for every key but ours
  (`IInputProcessor.h:26`), so no vanilla or mod binding is shadowed. Registration retries per frame
  until Slate exists; `Shutdown()` unregisters it, because a pre-processor left in Slate's list after the
  module unloads is a crash rather than a leak.
- **⚠ The old note said this was waiting on "the Game Instance Module's keybind registry".** SML's
  `UGameInstanceModule` **has no keybind registry** — `GameInstanceModule.h:30-83` lists everything it
  does have. The thing being waited for did not exist, so the wait was permanent. Comment corrected.
- **And the box cache stopped invalidating itself.** `ComputeEnvironmentKey` included every enabled
  plugin's `name@VersionName` **including FPM's own**, so every bump re-derived a cache whose contents
  do not depend on our version: the 0.4.1 boot logged `cache MISS->rebuilt | 3678 classes examined |
  0 from cache`. Derivation-logic changes already have their own deriver version. Our entry is skipped.
- **Plus `FPM.Hitch.Packages`** — the sync-loaded packages ranked by count, because the per-hitch `last=`
  field names whichever package finished last in its span and one measured span held 283 of them.
  Choosing a pin set from those names would rank by coincidence.
- **Verified:** build-only, `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 08:30 — VERSION — 0.4.0 → 0.4.1

- **What:** `VersionName` / `SemVersion` → `0.4.1`, `RemoteVersionRange` → `=0.4.1`.
- **Why PATCH and not MINOR:** no new console variable, no new diagnostic channel, no new fix. Every item
  below either REPAIRS an instrument 0.4.0 shipped hours earlier or completes one that was measurably
  half-blind. The repo's table reserves MINOR for new functionality; this is 0.4.0 finishing the job.
- **Verified:** build-only, `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 08:30 — CODE — close the instrument's own blind spot, name the sync load, and print the identity pair

Everything here was found BY the 0.4.0 boot, which is the argument for having shipped it.

- **⚠ THE HITCH METER WAS BLIND TO THE EXACT THING IT WAS BUILT FOR.**
  `FCoreDelegates::OnAsyncLoadingFlush` is broadcast only when `ThreadContext.SyncLoadUsingAsyncLoaderCount
  == 0` (`AsyncLoading2.cpp:11171-11176`) — the engine's comment says *"if the sync count is 0, then this
  flush is not triggered from a sync load"*. A `LoadAsset_Blocking` stall **is** a sync load, so it never
  fired that delegate. `m6249889` is entirely about blocking loads, so 0.4.0's `0 of them had an async-load
  flush` was a PARTIAL answer that read like a complete one. **This is the fourth instrument gap of the run
  and the first one I built myself** — I read that guard line while writing the file and did not act on it.
  Now also subscribes to `FCoreDelegates::OnSyncLoadPackage` (`CoreDelegates.h:117`, broadcast
  `UObjectGlobals.cpp:1742`/`:1815`), counted and reported SEPARATELY — folding the two together would hide
  which fired. Unlike the flush delegate it is handed the **package name**, so a hitch line now says
  `SYNC LOAD(S), last='<package>'` at level 1. That is `m6249889`'s literal next step, no longer needing a
  verbose boot.
- **GC telemetry**, via `GetPreGarbageCollectDelegate()` (`UObjectGlobals.h:3343`). The 0.4.0 boot measured
  92 client hitches of which only 33 had a swapchain resize before them; GC is the standing candidate for
  the rest (`m6253024`: GC is skipped while async loading, `UnrealEngine.cpp:2017`, so a due pass fires the
  instant streaming quiets — i.e. while moving). Counts only. **The pacing lever is deliberately NOT built:
  it needs a cvar-writing surface FPM2 does not have, and it must not be chosen before this measurement
  exists.**
- **The residency fix pinned too late to ever work.** Measured: the pin landed at `05:53:36.879` (frame 748,
  game-world load) while both real user-icon prints fired at **frame 0**, `05:53:17`, beside
  `Widget_ServerManager` and `mCreateNewGame` — **main-menu** widgets. It arrived 19 s after the only
  occurrence it had to beat. A world-load hook cannot fix that (the menu scene never reaches FPM2's
  dispatch), so it now retries every frame from `Arm()` until the asset manager exists, then unregisters.
  The not-ready line is logged **once**, not once per frame — the first version would have flooded the log
  it writes to.
- **⚠ I contaminated my own measurement.** The residency log line contained the literal token
  `[BPW_UserIcon]`, so it matched every grep for the widget's prints and inflated the count by one per
  session — and I then read the contaminated count. The token is gone; the line describes the widget
  without spelling its tag. **An instrument must not appear in its own results.**
- **The clone sensor now prints WHICH identities, not just how many.** `onlineAccountIds=2` carried the
  investigation for a day and could never say which two. Added `FPMCloneFormatAccountIds` using
  `LexToString(EOnlineServices)` / `ToLogString(FAccountId)` (`CoreOnline.h:292`, `:341`). **And a matching
  candidate now prints at level 1** instead of verbose-or-nothing — on Ant's save that is 1-2 lines against
  61-62, so the duplicate PAIR, which is the entire finding, no longer requires a deliberate boot nobody
  had reason to run until after the damage.
  Motivation, from the game's own header (`ClientIdentification.h:17-19`): *"if one online id matches, the
  identities are considered to match."* A joiner carrying Steam AND Epic matches any state saved under
  either. Measured 2026-08-09: two joins ten minutes apart in ONE server process bound two DIFFERENT states
  (`BP_PlayerState_C_2147450115` then `..._2147448825`) and Ant's outfit changed colour with them.
- **Files:** `Core/FPMHitchMeter.{h,cpp}` · `Streaming/FPMAssetResidency.{h,cpp}` ·
  `Fixes/Vanilla/FPMCloneSensor.cpp` · `FicsitsPerformanceManager.Build.cs` (adds `CoreOnline` — declared
  after the link failed on exactly those two unresolved externals, not assumed transitive).
- **Revert:** each item is independent; the Build.cs line is only needed by the clone-sensor change.
- **Verified:** build-only — `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 07:35 — CODE — review pass on 0.4.0: three findings, all fixed before it was ever packaged

Verdict was **NEEDS REVISION** — 0 blockers, 1 High, 2 Medium. Ant's standing rule is that the version bump
is the trigger and the last cheap moment; this is that pass paying for itself. Fixed in place: 0.4.0 was
never packaged or published, so the tag moves rather than a 0.4.1 being minted for pre-release churn.

- **HIGH — the meter dropped flush attribution for its worst cases.** `if (SpanMs >= CeilingMs)
  { ++LoadStalls; }` never looked at the flush count, though it had been computed one line earlier. So any
  span over `IgnoreAboveMs` (1000 ms) was bucketed as a stall and lost its flush coincidence **permanently**
  — and the severe tail is exactly where a synchronous load is most likely to BE the mechanism. The header
  claims "ATTRIBUTION, NOT ADJACENCY"; the statistic silently stopped covering the cases that claim lives or
  dies on. Now `LoadStallsWithFlush` is counted and reported beside the stall count.
- **MEDIUM — the meter could MISS a hitch that coincided with a world load.** `OnWorldLoad` set
  `bPrimed = false` without grading the open span first, so `Tick()` took its `!bPrimed` branch and reported
  nothing for it: neither hitch nor stall. It is dispatched from the game world module's CONSTRUCTION phase
  on the same game thread (`RootGameWorld_FicsitsPerformanceManager.cpp:61-64`), so it can land inside an
  unticked span. Grading was extracted into one `ClassifySpan()` that both paths call, so the two cannot
  disagree again.
- **MEDIUM — a comment claimed a guarantee the code did not give.** `FPMDiag.cpp`'s `static_assert` compares
  only the COUNT of `GChannelCVars` against `EChannel::Count`, while the comment said "this is the whole
  reason the indexing is safe". Adding two channels in the wrong ORDER keeps the count equal, passes the
  assert, and reports one channel's level under another's name. The comment now says what the assert
  actually checks, and `FPM.Diag.List` cross-checks each entry against its registered console name
  (`IConsoleManager.h:1104`) and logs an Error on a mismatch — a runtime order check where a compile-time
  one is not available.
- **SPEC DRIFT, recorded rather than argued away:** the review flagged `FFPMAssetResidency` as unasked-for
  scope, and it is right about the moment it was built — Ant asked for the instruments, and the residency
  fix is a behaviour fix found mid-task in a shelved note. She has since ratified it, but it went into the
  same bump with no separate review point, and that is the part worth not repeating.
- **Files:** `Core/FPMHitchMeter.{h,cpp}`, `Core/FPMDiag.cpp`.
- **Verified:** build-only — `Result: Succeeded`. **NOT boot-tested.**

## 2026-08-09 07:30 — VERSION — 0.3.1 → 0.4.0

- **What:** `VersionName` / `SemVersion` → `0.4.0`, `RemoteVersionRange` → `=0.4.0`, and the `Description`
  reworded from seven items to nine (asset residency under REPAIRS, hitch meter under DIAGNOSTICS).
- **Why:** MINOR, not patch. Two new capabilities — a frame-time instrument that did not exist in this game
  at all, and a new repair — plus two new diagnostic channels and four new console variables. The repo's own
  SemVer table bumps MINOR for new functionality in the `0.y.z` band; PATCH is for repairing an existing fix.
- **This also clears the tag drift Ant was blocked on.** `stamp_version.py --check` was correctly refusing
  because HEAD had moved past tag `0.3.1` (`git describe` → `0.3.1-3-g8edd155`). Tagging `0.4.0` at this
  commit restores SemVersion == git tag == generated pin.
- **Files:** `FicsitsPerformanceManager.uplugin`, `CHANGELOG.md`.
- **Revert:** set all three fields back and delete the tag. Never published.
- **Verified:** build-only. **NOT boot-tested.** Per Ant's standing rule the bump triggers a review pass
  before packaging.

## 2026-08-09 07:20 — CODE — asset residency: vanilla blocking-loads a platform icon nobody holds a reference to

- **What:** `FFPMAssetResidency` (`Streaming/FPMAssetResidency.{h,cpp}`) + a `FPM.Diag.Residency` channel. It
  asynchronously loads four vanilla platform-service icons (`TXUI_Steam_128`, `TXUI_Epic_128`,
  `TXUI_XBOX_128`, `TXUI_PlayStation_128`) and holds the streamable handle, so vanilla's own
  `LoadAsset_Blocking` finds them already resident. **No hook, no cvar, no ini, no content override,
  nothing written anywhere.** ~350 KB of RAM. Client only.
- **Why:** `Widget_PlayerList_Item::OnListItemObjectSet` → `BPW_UserIcon::SetServiceIcon`
  (`BPW_UserIcon.cpp:308`) calls `UKismetSystemLibrary::LoadAsset_Blocking` on a **soft** reference
  (`BPW_UserIcon.json:2827` `SoftObjectProperty`; the four paths at `:18-47`), and nothing in the game holds
  a hard one — so every player-list bind loads the package **synchronously on the game thread**. Measured
  2026-08-02: **17 of 26 `FlushAsyncLoading` lines share a frame number** with the `[BPW_UserIcon]` print
  emitted two statements earlier, and every governor window containing one reported a 383-405 ms worst
  frame. `LoadSynchronous` is `Get(); if (null && !IsNull()) TryLoad();` (`SoftObjectPtr.h:82-93`) and
  `Get()` ends in a `FindObject` **lookup, never a load** (`SoftObjectPath.cpp:886-891`) — so a resident
  texture skips the blocking branch outright.
- **⚠ THIS WAS ROOT-CAUSED ON 2026-08-02 AND SAT UNBUILT FOR A WEEK.** Recovered by the scratchpad audit
  (`SCRATCHPAD-AUDIT-2026-08-09.md`, top row of LIVE AND UNSHIPPED) from
  `patches-2026-08-02/fpm-patch-asyncload.md`. Every claim in it was re-verified at bytes before this was
  written: the four asset paths, the `SoftObjectProperty` type, and `FStreamableManager : public FGCObject`
  (`StreamableManager.h:702`, `AddReferencedObjects` `:873`).
- **Two departures from the source note, both deliberate:** (1) it is an `IFPMFix`, not a
  `UGameInstanceSubsystem` — FPM2 has a fix contract and the note predates it; the pin is attempted at
  `Arm()` and again at `OnWorldLoad()` because the asset manager's readiness at module-startup is not
  assertable from here. (2) its `UPROPERTY` hard-reference array is **not** carried: the live streamable
  handle already keeps the assets GC-referenced (receipt above), and duplicating that buys a second thing
  that can rot.
- **⚠ One correction to the note, not carried silently:** it calls these "pause/ESC menu" hitches, but the
  context lines it cites — `UpdateFocusHighlights [mCreateNewGame]`, `Widget_ServerManager NO CDO` — read as
  **main-menu** widgets. Same widget, same soft reference, same fix either way; the label is just not
  settled and should not be repeated as if it were.
- **Files:** `Public/Streaming/FPMAssetResidency.h` (new) · `Private/Streaming/FPMAssetResidency.cpp` (new) ·
  `Public/Core/FPMDiag.h` + `Private/Core/FPMDiag.cpp` (channel) · `Private/FicsitsPerformanceManager.cpp`
  (arm). No `Build.cs` change — `Engine` is already a public dependency and UBT globs `Public`/`Private`
  recursively, so the new `Streaming/` folder needs no registration.
- **Revert:** delete the two files, drop the `Residency` enumerator + cvar + `ChannelName` case, remove the
  one `Arm` line. Nothing is written at runtime.
- **Verified:** **build-only** — `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`. **NOT boot-tested.** Boot check, falsifiable in BOTH directions:
  `[FPM] residency: 4/4 vanilla platform icons pinned` at startup, then open the menu several times — the
  `[BPW_UserIcon]` prints must **REMAIN** (the widget still runs) while the same-frame `FlushAsyncLoading`
  lines must be **GONE**. If the prints vanish too, something was suppressed that should not have been.

## 2026-08-09 07:10 — CODE — hitch meter: the engine's frame-time detector is compiled out of this game, so FPM brings its own

- **What:** `FFPMHitchMeter` (`Core/FPMHitchMeter.{h,cpp}`), plus a new `FPM.Diag.Hitch` channel and three
  cvars — `FPM.Hitch.ThresholdMs` (50), `FPM.Hitch.IgnoreAboveMs` (1000), `FPM.Hitch.SummarySeconds` (60) —
  and one console command, `FPM.Hitch.Report`. It samples wall clock once per engine tick, counts frames
  at or above the threshold, and reports **with its denominator**. It subscribes to
  `FCoreDelegates::OnAsyncLoadingFlush` (`CoreDelegates.h:105`) so each hitch line says whether an
  async-load flush happened *inside that frame*; at level 2 it also names the packages in flight via
  `FCoreDelegates::GetOnAsyncLoadPackage()` (`:115`). Arms on client **and** dedicated server. Installs no
  hook, writes no cvar, no ini, no save.
- **Why:** Ant, 2026-08-09, in-game on 0.3.1: *"game hitches when opening menus and such and sometimes when
  moving fast through the world"*, and earlier *"i got a big hitch a few min ago. maybe visible in logs"* —
  it was not, because **nothing in this build measures frame time.** `FGameThreadHitchHeartBeat` is guarded
  by `USE_HITCH_DETECTION` = `(ALLOW_HITCH_DETECTION && …)` (`Misc/Build.h:454`), `ALLOW_HITCH_DETECTION`
  defaults to `0` (`:439-441`) and is redefined nowhere in this engine tree; the shipped client's own
  `SharedDefinitions.Engine.Project.…h` carries `UE_BUILD_SHIPPING 1` / `WITH_EDITORONLY_DATA 0` and **no
  `ALLOW_HITCH_DETECTION` line at all**. So `-hitchdetection=50` and the `[Core.System]`
  `GameThreadHeartBeatHitchDuration` ini key are both **no-ops in the retail game** — the detector is absent
  from the binary, not switched off.
- **Two design points that are the whole difference between an instrument and a decoration:**
  (1) **Wall clock, not the delta the ticker is handed.** The core ticker is driven by
  `FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime())` (`LaunchEngineLoop.cpp:5852`), and that delta is
  smoothed and range-clamped (`UEngine::bSmoothFrameRate` / `SmoothedFrameRateRange`, `Engine.h:1552`,
  `:1564`). Smoothing exists precisely to hide spikes, so an instrument built on it would report a tidy
  number across the exact event it was built to catch.
  (2) **Every summary carries the frame count it was measured over**, so a dead meter reads as `0 in 0`
  rather than as a calm session. This is the fourth instrument-gap incident in two days — saturation
  (`LogNetTraffic` Verbose under a Warning default), rain (needed an older log to prove the category
  emits), hitches (no instrument at all). A zero that reproduces is still a zero that means nothing if the
  emitter never fires.
- **What it settles:** `m6249889` measured 54 blocking `FlushAsyncLoading` calls in ~14 min and two log
  signatures that always precede them — and correctly flagged that adjacency is not causation. The
  per-frame flush count converts that into a counted coincidence rate.
- **Files:** `Public/Core/FPMHitchMeter.h` (new) · `Private/Core/FPMHitchMeter.cpp` (new) ·
  `Public/Core/FPMDiag.h` + `Private/Core/FPMDiag.cpp` (new channel, in both places — the `static_assert`
  in the cpp enforces that) · `Private/FicsitsPerformanceManager.cpp` (arm). No `Build.cs` change: `Core`
  and `CoreUObject` are already public dependencies. No version bump — the integrator owns it.
- **Revert:** delete the two new files, drop the `Hitch` enumerator + its cvar + its `ChannelName` case, and
  remove the one `FPMFixes::Arm` line. Nothing is written at runtime, so there is nothing else to undo.
- **Verified:** **build-only** — `Build.bat FactoryEditor Win64 Development -Module=FicsitsPerformanceManager`
  → `Result: Succeeded`, compiled and linked. **NOT boot-tested.** The boot check is falsifiable: a summary
  line every 60 s carrying a non-zero frame count. If the frame count is 0, the meter is dead and every
  number beside it is void.

## 2026-08-08 19:40 — VERSION — 0.2.2 → 0.3.0

- **What:** `VersionName` / `SemVersion` → `0.3.0`.
- **Why:** MINOR, not patch. Two new capabilities: the schematic probe (a new fix) and `FPMDiag` (a new
  runtime surface with six console variables). SemVer bumps MINOR for new functionality in the `0.y.z`
  band; PATCH is for repairing an existing fix.
- **Files:** `FicsitsPerformanceManager.uplugin`, `CHANGELOG.md`.
- **Revert:** set both fields back. Never published.
- **Verified:** build-only. **NOT boot-tested.**

## 2026-08-08 19:40 — CODE — the schematic probe: measure the theory instead of guessing a fourth guard

- **What:** `FPMSchematicProbe`, LOG-ONLY on both `CanGiveAccessToSchematic` entry points
  (`UFGSchematic::` static, `FGSchematic.h:160`; `AFGSchematicManager::`, `FGSchematicManager.h:314`).
  No `Override`, no `Cancel`, no refusal — every answer is vanilla's. It counts calls, and logs
  **unthrottled** when a schematic has a NULL default object.
- **Why — Ant ruled "carry it with just diagnostics" and "check what is needed here". I checked, and the
  answer was NO GUARD.** Of 31 crash dumps, **6** carry `CanGiveAccessToSchematic` in the CALLSTACK (not
  merely in the log, which every dump does because the mod loads). All 6 are `EXCEPTION_ACCESS_VIOLATION`
  at `0x2c0`, all 6 on the GRANT path via `Internal_CommitCurrentSchematicTransaction`. **4 have an FPM
  frame; 2 do not; and one — `A981D1D4` — has neither FPM nor KPrivateCodeLib.** A crash that happens
  with no mod on the stack is a VANILLA crash.
- **So the retired guard's premise is dead.** It tested `GetDefaultObject(false) != nullptr` on the
  theory that `0x2c0` is a null-CDO read; that guard shipped and the crashes continued. The old file's
  own comment records the ruling: *"DO NOT narrow this guard again without understanding the refusal.
  That is now the third time that instruction has had to be written down in this file."* A fourth
  narrowing would be a fourth guess. This probe MEASURES the theory instead: if a `0x2c0` crash lands
  while the null-CDO counter is still zero, the theory is dead by measurement rather than by argument.
- **It emits an ARMED line** — the one thing the old override never had, which is precisely why "never
  fired in 13 sessions" could not be told apart from "never installed".
- **The hot path is protected.** `0.58.54` logged and flushed on every call and Ant reported *"the
  entire game freezes when opening the hub"*. Here the per-call cost is an atomic increment and a
  pointer compare; no string work, no I/O, unless something anomalous is found. Measured context: the
  export holds 623 `.json` in the Schematics tree, of which **611** are BlueprintGeneratedClasses whose
  Super is `FGSchematic` — that bounds how many schematics EXIST, and says nothing about call rate,
  which `GQueries` will answer on the first boot.
- **Files:** `Public/Fixes/Interop/FPMSchematicProbe.h`, `Private/.../FPMSchematicProbe.cpp`,
  `Private/FicsitsPerformanceManager.cpp`.
- **Revert:** drop `FPMFixes::Arm(FFPMSchematicProbe::Get())`.
- **Verified:** compiles clean; `FPM.Diag.Schematic` and `schematic-probe ARMED` present UTF-16 in the
  DLL. **NOT boot-tested.**

## 2026-08-08 19:40 — CODE — FPMDiag: diagnostics you can switch from the console

- **What:** `Core/FPMDiag.{h,cpp}` — a master `FPM.Diag` plus five channels (`FPM.Diag.Schematic`,
  `.Hologram`, `.Inventory`, `.Clone`, `.Overlay`) and `FPM.Diag.List` to print the effective state.
  Levels: `0` silent · `1` on · `2` verbose. Wired into all four fixes and the overlay.
- **Why:** Ant, *"we should add debug stuff to every feature we make in the mod so everything has a
  known log or whatever when it fails"* and *"diagnostics are good either way so we KNOW what breaks and
  why"*. Before this, the rain fix had two hand-rolled cvars and nothing else had any, so silencing a
  noisy log meant a rebuild.
- **A disabled channel never disables a FIX.** Only the printing stops; every counter still climbs, so
  `FPM.Diag.List` still reports after the fact. Turning diagnostics off must not change what the mod
  DOES, or a quiet log becomes evidence of nothing rather than evidence of calm.
- **One stated exception to "0 = silent": the ARMED line**, which fires from `StartupModule` before any
  console command could exist, and is the line that distinguishes "armed and saw nothing" from "never
  armed". Documented in the header — an unstated exception is a broken contract.
- **Found by review, twice.** Round 1: **seven** log sites ignored the switch the header promised, and
  `FPMOverlay::Post` bypassed it entirely. Round 2: `FPM.Diag.Clone` was a **dangling channel** —
  declared and wired to nothing, which is worse than dead code because setting it teaches you to
  distrust every other switch. Also reordered 8 gates so the cheap modulo short-circuits before the
  cvar read.
- **Files:** `Public/Core/FPMDiag.h`, `Private/Core/FPMDiag.cpp`, plus gating in the four fixes and
  `FPMOverlay.cpp`.
- **Revert:** the channels default to on, so deleting the gates restores previous behaviour exactly.
- **Verified:** compiles clean; `FPM.Diag`, `FPM.Diag.Schematic`, `FPM.Diag.Overlay`, `FPM.Diag.List`
  present UTF-16 in the DLL. **NOT boot-tested.**

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

