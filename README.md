# WNLPackFix

A native (C++) performance, graphics, and pack-stability companion mod for **Satisfactory**, built on the **Satisfactory Mod Loader (SML)**.

WNLPackFix does two things:

1. **A client-side perf + graphics governor** — native dynamic resolution routed through *whatever upscaler you already run* (DLSS / XeSS / FSR / TSR, auto-detected), a real-thread-time CPU-relief controller with a hard FPS floor, and a full-range quality-stage engine that treats **your own graphics settings as the floor** and only spends measured GPU headroom to go *above* them.
2. **A set of server/client pack fixes** — it gates a multiplayer RPC desync flood, repairs rubber-banding on factory pieces, synthesizes missing rain-occlusion data, silences a dedicated-server audio-log flood, and raises the navmesh tile ceiling so creatures can path a large modded map.

Everything is done with **composable SML native hooks and runtime CVars**. It modifies **no other mod's files** and ships **no game assets**.

- **Game:** Satisfactory 1.2.3.1 (CL 495413), UE 5.6.1-CSS
- **Loader:** SML `^3.12.0`
- **Owner / author:** DegradingAnt (Ant)
- **Network activity:** none (fully offline; the only external call is a local DXGI VRAM read on Windows)

> ### AI disclosure
> **This mod is AI-assisted. All code was reviewed by a human (the author) before release.** Development used an AI coding assistant (Claude) under human direction and review. This disclosure is intentional and permanent — it matches both the author's standing policy and the ficsit.app content policy. Please do not file PRs or edits that remove it.

---

## What it does

### Client perf + graphics governor (`WNLPerfGovernor`)

The governor runs **only on clients** (it no-ops on dedicated servers and in the editor). It arms once, waits for the game to settle, detects your GPU vendor and your **live** temporal upscaler, then drives three independent levers:

**Lever 1 — Native dynamic resolution.** Hands resolution scaling to the engine's own dynamic-resolution system (`r.DynamicRes.OperationMode=2`) instead of poking `r.ScreenPercentage` manually. The engine debounces changes and feeds whichever upscaler you have active, so scaling is **flash-free**. (An earlier manual `r.ScreenPercentage` controller glitched TSR into geometry flashing; the engine now ignores manual screen-percentage once dyn-res is on — see [Hard rules](#hard-rules--architecture-a-contributor-must-know).)

**Lever 1b — CPU relief.** Resolution and GPU-quality levers only help *GPU-bound* frames. This is the *CPU-bound* lever. Driven by **real thread time** (`GGameThreadTime` / `GRenderThreadTime` — what `stat unit` shows; FPS is only a fallback), it ramps a single smooth intensity `[0..1]` as the CPU approaches a hard FPS floor (default 75). That intensity drives a **visibility-ordered ladder**: invisible levers first (conveyor item rate / draw radius, far skeletal LODs), moderate ones next (building cull, effects density), and clearly-visible foliage cull **last and shallowest** (emergency band only). Fast ramp-up to defend the floor, slow ramp-down to avoid oscillation. It **may dip below your quality baseline** to hold the floor and restores fully on headroom.

**Lever 2 — The quality-stage engine.** One integer stage over the range **−4 .. +6**:
- **Stage 0 is your own settings** — vanilla performance is guaranteed; the mod never makes you slower than your menu choices unless the CPU floor is actively breached.
- **Positive stages (+1 .. +6)** are cumulative graphics *bonuses* up to beyond-Cinematic (Lumen / Nanite / VSM / shadow / fog levers). They are promoted **one at a time** only at near-full resolution with measured GPU headroom, each promote verified for a watch window. A "burn" (over budget inside the window) demotes, learns the stage's real cost, and starts a growing cooldown. Bonuses shed within seconds under load, *before* resolution is meaningfully spent.
- **Negative stages (−1 .. −4)** are an emergency cut ladder (contact shadows → fog coarsen → sparse-probe GI → GI tier last), engaged **only** at the resolution floor while GPU-bound, and restored LIFO the instant headroom returns or the frame goes CPU-bound.

The stage levers use **MaxOf / MinOf composition** against your captured baseline, so a bonus can only *raise* quality and a cut can only *lower* it relative to what you set — the two never fight, and stage 0 is an exact revert.

**Supporting behavior:**
- **Cap-aware budget** — the frame budget follows your live frame cap (`t.MaxFPS`) and a learned vsync plateau, so sitting at your monitor's refresh reads as "at the cap" (healthy), not as overload.
- **User-settings adoption** — menu changes can't reach the governor's `SetByConsole` CVars directly, so it polls `GameUserSettings` and adopts *your* changes (frame limit, resolution scale, GI quality) as new baselines.
- **Per-vendor guards** — NVIDIA (Reflex, keep the lower DLSS res floor), AMD (anti-flicker: `r.TSR.ShadingRejection.Flickering`, DF-shadow cull tiling, higher res floor, lower bonus ceiling), Intel Arc dGPU (capped bonus stages to avoid a Lumen+Nanite+XeSS DX12 crash class) and Intel iGPU (no bonus stages; let dyn-res drop hard to hold FPS).
- **NVIDIA Reflex** low-latency, **raw mouse** (disables UE's hidden mouse smoothing via the `UInputSettings` CDO so it survives config rewrites), and a VRAM-headroom gate that reacts to other apps holding VRAM.
- **Frame generation is never touched** (it works poorly in Satisfactory).

### Adaptive indoor-fog controller (`WNLFogController`)

Client-only. The flat blue distance-haze that hangs inside sealed player factories clears as you walk in, while **god-rays and outdoor atmosphere stay exactly as they are**. It moves only `ExponentialHeightFogComponent::StartDistance` (height fog), **never volumetric fog** — so light shafts keep pouring through windows and holes even in a sealed room. The fog-free bubble is sized to the *actual* nearest sealing wall (not a fixed radius), so a doorway keeps its correct outdoor view. Only `AFGBuildable` hits count as sealing, so natural cave/terrain ceilings keep their fog.

### Server / client pack fixes (`WNLPackFix.cpp`)

All are composable native hooks with tightly-scoped boundaries:

| Fix | Side | What it does |
|---|---|---|
| **StatsSign RPC gate** | authority | The Stats mod's sign buildables dispatch `EndingProduction`/`EndingConsumption` for actors with no owning connection ~2.5M times/session; the engine just logs and drops them. This gate reproduces the vanilla drop **without the log write**, scoped strictly to those two function names + the `Build_StatsSign` class prefix + the no-owner condition. Kills the dominant "server feels laggy" desync factor. |
| **Static-base movement fix** | server | Base-relative movement corrections on `UFGColoredInstanceMeshProxy` bases (immobile instancing proxies) are ignored by clients that can't net-resolve the base → rubber-banding on factory pieces. Rewrites those corrections to world-space. Elevators/vehicles keep relative basing. |
| **Rain-occlusion data fix** | all | Buildables without an authored `mRainOcclusionBoundingBox` spam `LogRainSystem` and let rain fall through them. Synthesizes the box from real mesh geometry as each piece loads (opts geometry-less helpers out entirely). Fixes the data instead of muting the log. |
| **Contact-shadow suppressor** | client | Suppresses screen-space contact shadows on the two classes that shimmer under them — moving belt items and alpha-masked foliage — while static geometry keeps its grounding contact shadows. |
| **Wwise server audio gate** | dedicated server only | A dedicated server has no audio device, so `UAkGameplayStatics::StopActor` always fails and logs (~3164 warnings/session). Armed **only** on the dedicated server, where the original is a guaranteed no-op. Clients never register it. |
| **Navmesh coverage fix** | server | CSS caps the navmesh at 65,536 tiles; a large modded map needs ~306,440, so ~79% of the map has no navmesh and creatures can't path there. Raises the per-navmesh-class tile ceiling to 524,288 by reflection. Memory-safe: only the 176 B/tile slot table grows (~+40 MB per active nav class); per-tile geometry stays World-Partition-streamed. |

---

## Install

### One-click (recommended)

Install with the **Satisfactory Mod Manager (SMM)** — search **WNL PackFix** and click Install. SMM resolves the SML dependency and updates automatically. The mod is client-usable even if the host doesn't have it (`RequiredOnRemote: false`).

### Manual

1. Install [SML](https://ficsit.app) (SMR resolves it for you if you use SMM).
2. Download the packaged `WNLPackFix.zip`.
3. Extract into your Satisfactory install so the plugin lands at:
   `<Satisfactory>/FactoryGame/Mods/WNLPackFix/`
   (the zip contains the `Windows/`, `WindowsServer/`, and `LinuxServer/` platform folders).
4. Launch. On first run the governor writes its default config (see below).

For a **dedicated server**, install the same mod on the server; the server-side fixes (RPC gate, static base, navmesh, Wwise gate) arm on the authority, and clients get the client-side governor/fog.

---

## Configuration

The governor writes a JSON config with all defaults on first run at:

```
<Satisfactory>/FactoryGame/Configs/WNLPackFix.cfg
```

Every value is hot-tunable without a rebuild. A hand-edited file is **clamped on load** (bad values are corrected, not honored); an *unparseable* file is **kept as-is** (never overwritten) so you can fix it. Two console commands are also available:

- `WNLPackFix.Status` — print the live governor state (vendor, active upscaler, resolution %, current stage, CPU-relief intensity).
- `WNLPackFix.Set <Key> <Value>` — persist any top-level key and live-apply the safe ones. Unknown keys are rejected (typo guard).

An in-game config **menu** (SML config page) is also exposed; menu values are merged over the file before parsing, so what you see in the menu is what applies.

### Config reference

Ranges below are the on-load clamp bounds. Anything out of range is silently corrected to the nearest bound.

#### Core

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `Enabled` | bool | `true` | — | Master switch for the whole governor. |
| `TargetFPS` | float | `90` | 30–240 | **Soft** FPS target. Dynamic res and bonus promotes steer toward this. |
| `MinScreenPct` | float | `58` | 25–100 | Dynamic-resolution floor (% screen). 58 ≈ DLSS-Balanced input; lower reads as global shimmer. Vendor floors may raise this. |
| `MaxScreenPct` | float | `100` | MinScreenPct–100 | Upper resolution bound (100 = DLAA-grade input when there's headroom). |
| `CapFPS` | float | `120` | 0, or 30–480 | `t.MaxFPS` cap, applied only if the game is otherwise uncapped. `0` = leave uncapped. The internal headroom line is derived from this so the two can't desync. |

#### Quality-stage engine

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `MaxBonusStage` | int | `6` | 0–6 | Bonus ceiling (+6 = beyond-Cinematic). `0` disables bonuses. Auto-lowered on AMD / Intel. |
| `MaxCutStage` | int | `4` | 0–4 | Emergency cut floor (−4 = GI tier down). `0` disables cuts entirely. |
| `NudgeAfterSec` | float | `5` | 0.5–300 | Sustained overload at the res floor before each cut rung engages (fast-down). |
| `RestoreAfterSec` | float | `8` | 0.5–300 | Recovery hold before each LIFO cut restore (slow-up, no oscillation). |
| `PromoteDwellSec` | float | `30` | 5–600 | Continuous headroom required before each bonus promote (slow-up). |
| `DemoteDwellSec` | float | `1.5` | 0.5–60 | Load hold before a bonus is shed (fast — defend the target). |
| `PromoteGpuFrac` | float | `0.80` | 0.5–0.95 | Promote only while GPU time is below this fraction of the frame budget. |
| `PromoteHeadroomFactor` | float | `1.5` | 1–5 | And only while headroom exceeds this × the next stage's (learned) cost. |
| `VerifyWindowSec` | float | `8` | 2–60 | Post-promote watch window; over budget inside it = a "burn" (demote + learn + cooldown). |
| `PromoteCooldownSec` | float | `120` | 10–3600 | First re-promote cooldown per burned stage (doubles on repeat, capped below). |
| `PromoteCooldownMaxSec` | float | `600` | PromoteCooldownSec–7200 | Cap on the growing per-stage cooldown. |
| `LadderGraceSec` | float | `45` | 0–600 | No promotes/cuts for this long after the graphics pass / each baseline re-capture (the join / PSO-compile storm). LIFO restores are exempt. |

#### Baseline-free set (applied once at post-settle; measured-free or VRAM-only)

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `VSMStability` | bool | `true` | — | VSM page-pool + cache + panning bias (anti-shimmer, no ms cost). |
| `ReflectionDenoise` | bool | `true` | — | Lumen reflection screen-space reconstruction (measured no-op-to-free). |
| `ContactShadows` | bool | `true` | — | Force static-geometry grounding contact shadows (belt items + foliage are suppressed separately). |
| `ContactShadowLength` | float | `0.035` | 0–0.1 | Screen-space contact-shadow length (>0.1 = peter-panning streaks). |
| `NaniteSharpen` | bool | `true` | — | Enable the `r.Nanite.MaxPixelsPerEdge` tweak. |
| `NanitePixelsPerEdge` | float | `1.0` | 0.25–8 | 1.0 = default; `<1` sharper geometry, `>1` cheaper. |
| `LumenPerfLevers` | bool | `true` | — | Lumen reflection trace-compaction + radiance-cache trace-tile sort (pure compute scheduling, no image change). |
| `DontLimitOnBattery` | bool | `true` | — | `r.DontLimitOnBattery=1` — trivial anti-throttle (free on laptops). |
| `MaxFrameQueue` | int | `1` | 0–3 | `RHI.MaximumFrameLatency` probe (CVar is absent on 1.2.3.1; kept in case CSS registers it). `0` = don't touch. |
| `ReflexMode` | int | `1` | 0–2 | `t.Streamline.Reflex.Mode`: 0 off / 1 low-latency / 2 +boost. NVIDIA-gated. Mode 2 pins GPU clocks and costs FPS when GPU-bound (opt-in). |
| `FSRSharpness` | float | `0.5` | 0–1 | `r.FidelityFX.FSR.Sharpness` on the AMD/FSR path. |
| `AsyncTick` | bool | `true` | — | Game-thread relief: async tick dispatch/cleanup + FX batching. |
| `GrassTickInterval` | int | `10` | 0–60 | Grass refresh every N frames (CPU relief, near-free). |
| `GrassDensityScale` | float | `1.0` | 0.5–4 | Static opt-in only; changing mid-session rebuilds grass population. `1` = untouched. |
| `StreamingPoolMB` | int | `0` | 0–16384 | Texture streaming pool. `0` = auto by VRAM (≥16 GB → 6144, ≥12 GB → 4096); `>0` = explicit. |
| `IncrementalGC` | bool | `true` | — | Spread GC reachability/gather over frames (experimental engine flag; gated for bisectability). |

#### Vendor-adaptive guards

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `VendorAdaptive` | bool | `true` | — | Master switch for the per-GPU branch. |
| `AmdMinScreenPct` | float | `62` | 40–90 | Higher dyn-res floor for FSR/TSR/None (they blur worse than DLSS at low res). |
| `TsrXessSharpen` | float | `0.8` | 0–2 | `r.Tonemapper.Sharpen` on non-DLSS paths (never stacked on DLSS). |
| `ForceUpscalerIfNone` | bool | `true` | — | Force TSR (AMD/Other) when the user has no temporal upscaler active. |
| `UpscalerAutoSelect` | bool | `true` | — | Branch tuning on the **active upscaler** (DLSS/XeSS/FSR/TSR), not the vendor. |
| `AmdAntiFlicker` | bool | `true` | — | AMD: `r.TSR.ShadingRejection.Flickering=1` + TAA→TSR (RDNA Lumen flicker). |
| `AmdDFShadowCullTile` | float | `400` | 100–2000 | AMD: `r.DFShadowCullTileWorldSize` (terrain / DF-shadow flicker). |
| `IntelArcMaxBonusStage` | int | `2` | 0–6 | Arc dGPU bonus ceiling (Lumen+Nanite+XeSS DX12 crash class on 5.6). |
| `IntelIGpuMaxBonusStage` | int | `0` | 0–6 | Iris/UHD iGPU: no bonus stages. |
| `IntelIGpuMinScreenPct` | float | `50` | 25–80 | iGPU: let dyn-res drop hard to hold FPS (opposite of the dGPU floor). |
| `AssertRayReconstructionOff` | bool | `true` | — | RR is inert on software Lumen here → hard-guard `DenoiserMode=0`. |
| `GpuBoundFraction` | float | `0.85` | 0.5–0.99 | GPU time ≥ this fraction of frame time = GPU-bound (cuts allowed). |
| `VramFloorMB` | float | `1500` | 256–8192 | Keep this much VRAM free; below it, don't promote (reacts to other apps). |
| `PoolBudgetFraction` | float | `0.4` | 0.1–0.8 | Streaming pool = this fraction of the OS-**granted** VRAM budget (off budget, not free headroom → no self-feedback loop). |

#### CPU-relief controller

| Key | Type | Default | Range | Meaning |
|---|---|---|---|---|
| `CpuRelief` | bool | `true` | — | Master switch for the smooth CPU-relief controller. |
| `CpuFloorFPS` | float | `75` | 30–120 | **Hard** floor to defend — relief goes maximal at/below this. |
| `CpuComfortFPS` | float | `90` | CpuFloorFPS+5 – 240 | No CPU throttle while the CPU can make this (the soft target). |
| `CpuBuildCullMin` | float | `0.65` | 0.3–1 | Band B: FG building-cull modifier at full relief (`1.0` = game default). |
| `CpuFoliageCullMin` | float | `0.85` | 0.3–1 | Band C (emergency, shallow): foliage cull-distance scale at full relief. |
| `ConveyorItemFreqMin` | int | `24` | 5–120 | Band A: `FG.ConveyorItemFrequency` at full relief (60 = game default). |
| `ConveyorDrawDistMin` | float | `40000` | 10000–200000 | Band A: `CSS.Conveyor.MaxDrawDistance` at full relief (100000 default). |
| `NiagaraQualityMin` | int | `2` | 0–4 | Band B: `fx.Niagara.QualityLevel` at full relief (3 = Ultra default). |

#### Fog (nested `"Fog"` object)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `Enabled` | bool | `true` | Master switch for the indoor-fog controller. |
| `IndoorStartDistance` | float | `12000` | **Cap** on the fog-free bubble radius (cm). The bubble is usually the nearest sealing wall. |
| `TransitionSec` | float | `4` | Fade length both directions (so fog "clears as you enter" rather than hard-cutting). |
| `RoofTraceUp` | float | `4000` | How far up (cm) to look for a roof — minimum trace length. |
| `CheckInterval` | float | `0.25` | Enclosure re-check cadence (s); the fade itself runs every tick. |
| `MinBubble` | float | `200` | Floor (cm) so a tiny closet never sets a ~0 bubble. |
| `WallBias` | float | `0.9` | Bubble = nearest wall × this, so height fog resumes just **inside** the shell. |
| `GrowLerp` | float | `0.2` | Slow grow (stops the bubble pulsing bigger on a lucky far reading). |
| `ShrinkLerp` | float | `0.6` | Fast shrink (collapse instantly when a doorway/near wall appears). |
| `SealCountMin` | int | `3` | Fewer sealing-ray hits than this → treat as open; the game owns the fog. |

> **Legacy keys** (`QualityNudge`, `QualityBonus`, `LumenCinematicBaseline`, `VSMUplift`, `VolumetricFogUplift`, `LumenFinalGatherUplift`, `SoftwareLumenMax`, `BonusAfterSec`) from pre-0.9 configs are migrated into the stage engine on load and logged as ignored. Remove them from hand-edited files.

---

## Build from source

WNLPackFix builds like any SML C++ mod — it is a UE plugin that lives under the SML dev environment's `Mods/` folder.

### Prerequisites

- The **Satisfactory modding dev environment** set up per the [SML C++ setup guide](https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/index.html): the CSS-patched Unreal Engine (**UE 5.6.1-CSS**), Visual Studio 2022 with the C++ / game-dev workloads, and the `SatisfactoryModLoader` starter project.
- This mod checked out at `SatisfactoryModLoader/Mods/WNLPackFix/`.

### Compile (development / iterate)

1. Regenerate the Visual Studio project files for the `FactoryGame.uproject` (right-click the uproject → *Generate Visual Studio project files*, or run UBT's `-projectfiles`).
2. Build the **Development Editor** target for `FactoryGame` to compile the mod for in-editor iteration, or the **Shipping** target for a runtime `.dll`.

The SML repo's `Build.bat` / the standard UBT invocation both work:

```bat
:: from the SatisfactoryModLoader root
Build.bat WNLPackFix
```

or directly via UBT:

```bat
"<UE>/Engine/Build/BatchFiles/Build.bat" FactoryGameSteam Win64 Shipping ^
    -Project="<path>/FactoryGame.uproject" -Module=WNLPackFix
```

### Package for release (Alpakit / RunUAT)

Packaging produces the single merged `WNLPackFix.zip` (top-level `Windows/ WindowsServer/ LinuxServer/`) that SMR ingests. Use the **Alpakit** panel in the editor (*Package* the WNLPackFix mod), or run the same step Alpakit wraps:

```bat
"<UE>/Engine/Build/BatchFiles/RunUAT.bat" PackagePlugin ^
    -Project="<path>/FactoryGame.uproject" -PluginName=WNLPackFix -Merge
```

This cooks and stages **all three targets** (Windows client, Windows server, Linux server). All three must build for a valid SMR upload.

> `dxgi.lib` is linked **Windows-only** (client VRAM query); the server targets don't touch it — see `WNLPackFix.Build.cs`.

---

## Hard rules / architecture a contributor must know

These are load-bearing invariants learned from real boot regressions. **Read before changing the governor.**

1. **Never re-add synchronous occlusion-culling CVars, and never raise the runtime Nanite buffer-cap at runtime.** Both caused visible **flashing**. Async occlusion research is fine; the *sync* version flickers. Nanite buffer caps are a boot-time-only concern, not a runtime lever.
2. **Never poke `r.ScreenPercentage` manually while native dynamic resolution is on.** The engine ignores it once `r.DynamicRes.OperationMode=2`, and the old manual slew controller bypassed the upscaler's hysteresis → geometry flashing. Resolution smoothing belongs to the engine now; the governor only sets it up once and reads the live fraction.
3. **The graphics floor is the user's own settings. Stage 0 = the player's baseline, always.** Every stage lever composes with **MaxOf** (bonuses can only raise) or **MinOf** (cuts can only lower) against a captured baseline. A bonus that could make a frame *slower* than the user's setting, or a cut that raises quality, is a bug. Stage 0 must be a bit-exact revert.
4. **Cuts are floor-only, GPU-bound-only, and LIFO.** Negative stages engage only at the resolution floor while GPU-bound, and restore last-in-first-out the instant headroom returns or the frame goes CPU-bound. Don't let cuts fire on CPU-bound frames (they buy nothing there — that's what the CPU-relief lever is for).
5. **CPU relief is a separate lever from GPU stages.** It's driven by real thread time, not FPS. Its ladder is visibility-ordered: invisible → moderate → foliage-last-and-shallow. Do not deepen the foliage band; a boot-test found aggressive foliage cull "looks terrible."
6. **Tune on the active upscaler, not the vendor.** An AMD user may be on TSR, XeSS, *or* FSR. Detect the live upscaler at post-settle. Never stack tonemapper sharpen on DLSS (it conditions its own image).
7. **Frame generation is never touched.** It works poorly in Satisfactory. This is deliberate.
8. **All hooks are composable (`SUBSCRIBE_METHOD` / `_VIRTUAL`), never `@Overwrite`-style replacements, and never armed in the editor** (`if (!WITH_EDITOR)` — arming native hooks at editor time can crash per SML docs). Prefer large hook targets; tiny functions corrupt under funchook (a 0.3.0 crash came from hooking a tiny `IsDynamicBase`).
9. **Server/client self-guarding is mandatory.** Renderer/audio/PlayerController paths must guard `IsRunningDedicatedServer()`. Server-authoritative fixes run on the authority; client visual/perf levers run on clients. Don't use `GetPlayerController(0)` for authority.
10. **No network activity, ever.** The only external call is a local Windows DXGI VRAM read. Do not add HTTP/sockets/telemetry — it would break the "no network activity" content-policy declaration.

---

## Credits

- **Author / owner:** DegradingAnt (Ant).
- **Satisfactory Mod Loader (SML)** and the CSS modding toolchain — the hooking/config/packaging foundation this mod is built on.
- **TajsGraph** (*Taj's Graphical Overhaul*, by TajemnikTV) — several of the *baseline-free* Lumen/CPU compute-scheduling levers (reflection trace-compaction, radiance-cache trace-tile sort, async-tick relief, grass tick interval) were inspired by techniques observed in TajsGraph. No code or assets were taken; the implementation here is original and the *technique inspiration* is credited.
- Built with AI assistance (Claude), fully human-reviewed — see the [AI disclosure](#ai-disclosure) above.

## License

[GPL-3.0](./LICENSE) — forks and derivatives stay open source. Copyright (c) 2026 DegradingAnt.

## Contributing

See [`CONTRIBUTING.md`](./CONTRIBUTING.md) — in particular the build/boot-test discipline and the hard rules above.
