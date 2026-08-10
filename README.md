<div align="center">

<img src="docs/branding/NOX_Mod_Logo_1024.png" alt="NOX — FICSIT Performance Manager" width="320">

# FICSIT Performance Manager

![Satisfactory 1.2.3.1 CL 495413](https://img.shields.io/badge/Satisfactory-1.2.3.1%20%C2%B7%20CL%20495413-E59344?style=flat-square)
[![SML ^3.12.0](https://img.shields.io/badge/SML-%5E3.12.0-E59344?style=flat-square)](https://ficsit.app/mod/SML)
[![License GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-E59344?style=flat-square)](LICENSE)
![Status: alpha](https://img.shields.io/badge/status-ALPHA-D6473A?style=flat-square)
![Network activity: none](https://img.shields.io/badge/network%20activity-none-3A3C40?style=flat-square)

**Repairs, guards and diagnostics for known Satisfactory bugs.**

[Hard rules](#hard-rules) · [Diagnostics](#diagnostics) · [Build from source](#build-from-source) ·
[Report a bug](https://github.com/DegradingAnt/Ficsit-performance-manager-/issues)

</div>

---

FPM repairs known multiplayer, rendering and stutter bugs. Each repair is a narrow native hook. When a
bug cannot be repaired yet, FPM measures it instead, so the cause can be named later.

> ## ⚠ ALPHA BUILD
>
> This mod is in active development. Expect bugs. Expect settings to move between versions. **Back up
> your save before you run it, and do not use it on a world you cannot replace.**
>
> FPM does run on a live dedicated server and a live client every day, so it is not untested. It is
> also not finished. **This build has no performance governor** — that work comes later. Today the mod
> ships repairs, guards and diagnostics.

---

## What it does

Each fix is one class in one file. Each fix must declare four things before it will compile: its
**name**, the **side** it may arm on, the kind of **claim** it makes about the bug, and where its
**diagnostics** go. The third one is unusual. Read [Origin status](#origin-status) below.

| Fix | Side | Origin status | What it does |
|---|---|---|---|
| `static-base` | any | **OriginNamed** | Clients cannot resolve movement corrections that are relative to an immobile instancing proxy, so players rubber-band on factory pieces. FPM rewrites those corrections to world space. Elevators and vehicles keep relative basing. |
| `rain-occlusion` | client | **OriginNamed** | Buildables with no authored rain-occlusion box fill the log with errors, and rain falls through them. FPM builds the box from real mesh geometry as each class loads. It repairs the data instead of muting the log. |
| `wwise-server-gate` | server only | **OriginNamed** | A dedicated server has no audio device, so one Wwise call always fails and writes a warning. One measured session held 681 of them. FPM cancels that call on a server and leaves a client alone. |
| `zipline-volume` | client | **OriginNamed** | A volume lever for the zipline sound bus. The default of `1.0` writes nothing, so vanilla stays bit-identical until you move it. |
| `asset-residency` | client | **OriginNamed** | The vanilla player list loads a platform icon from disk every time it binds, and it blocks the game thread to do it. FPM keeps four vanilla textures resident, so the vanilla code finds them and skips its blocking path. |
| `no-owner-rpc-gate` | any | Guard | Buildables send remote calls for actors that have no owning connection. The engine logs each one and drops it. FPM drops them without the log write. |
| `rail-connection-guard` | any | Guard | An unwired rail connection gets `nullptr` instead of an assert. Measured at 1,900 to 2,550 averted asserts per server start. |
| `hud-hook-guard` | client | Guard | Removes one third-party HUD hook descriptor that crashes the player HUD. It removes that class only. Two earlier and wider versions of this idea cost Ant her mod UI. |
| `texture-pool-guard` | client | Guard | The vanilla streaming pool is a flat 1000 MB and starves a large card. FPM sizes the pool to the card. It writes nothing for the first 45 seconds, and nothing at all below the card tier. |
| `wire-null-guard` | any | Guard | Removes null entries from power-circuit wire arrays before the autosave walks them. This state crashed the dedicated server inside its own autosave on 2026-08-09. |
| `schematic-null-guard` | any | Guard | Refuses a schematic access query when vanilla is about to read a null event subsystem. This is the largest crash-to-desktop class in the project dump corpus. |
| `save-settings-guard` | any | Guard | Stands between a temporary console-variable write and a permanent change to your own settings file. |
| `inventory-init` | any | ChokePointRepair | Sizes an uninitialized inventory instead of refusing items into it. |
| `hologram-net` | client | ChokePointRepair | Rebuilds the attachment points of a replicated build preview instead of skipping its setup. |
| `navmesh-ceiling` | any | ChokePointRepair | Raises the navmesh tile ceiling, then **reads the value back**. The earlier version logged a raise that the engine ignored. |
| `hitch-meter` | any | *UnknownCause* | Measures frame time. This game build compiles the engine hitch detector out, so without this there is no instrument at all. |
| `clone-sensor` | any | *UnknownCause* | Watches player-state matching at join time. Log only. |
| `schematic-probe` | any | *UnknownCause* | Watches both schematic-access entry points. It overrides nothing and changes no answer. |

### Origin status

The word *"fixed"* drifts. People use it for work that repaired a **symptom** at a convenient place and
never named the **cause**. Each case reads as reasonable on its own. The whole set then claims more than
it earned.

A fix cannot compile until it declares one of four values:

- **OriginNamed** — we identified the cause and we have the receipt. This is the only value that permits the word "fixed".
- **ChokePointRepair** — the repair happens at the earliest point we can reach. The cause is still unnamed.
- **Guard** — the harm is prevented. The cause belongs to somebody else, such as another mod or the engine.
- **UnknownCause** — the symptom is handled and the mechanism is unknown. This value gets the most scrutiny, and it must carry a diagnostic.

Any value except `OriginNamed` **owes an origin-naming diagnostic**. That is a channel whose stated job
is to name the cause from play data. This is why the mod ships instruments next to repairs.

---

## Requirements

| | |
|---|---|
| Game | Satisfactory **1.2.3.1**, CL **495413** |
| Loader | **SML `^3.12.0`** |
| Base plugins | AbstractInstance and Wwise. Both ship with the game. |
| Optional | Cartograph. FPM only detects it, to leave room in the texture pool. |

**Update the client and the dedicated server together.** FPM sets `RequiredOnRemote`, and SML then
requires the two versions to match exactly. If you update one side alone, the server refuses the join.
This is deliberate. Every fix here can act on the server, and a version split would give the two
machines different behavior without telling you.

---

## Diagnostics

Nothing needs a rebuild to inspect. Every diagnostic is a console variable or a console command, and
every name starts with `FPM.`. Run `FPM.Diag.List` to print all of them with their live values.

| Command | What it answers |
|---|---|
| `FPM.Version`, `FPM.Side` | Which build runs, and on which side. |
| `FPM.Fixes`, `FPM.Hooks` | Each armed fix with its side, origin status and channel. Each installed hook with its owner. |
| `FPM.Diag.List` | Each diagnostic channel and its effective level. |
| `FPM.Support` | A support bundle you can copy out of the console. |
| `FPM.Residue`, `FPM.ResidueDrill` | Audits the zero-residue claim from inside the game. |
| `FPM.CVars`, `FPM.CVarSnap`, `FPM.CVarDiff` | What FPM holds now, and what changed. |
| `FPM.Changes` | Every value this session that FPM is responsible for. |
| `FPM.CrashStamp` | The keys FPM wrote into the crash context at startup. |
| `FPM.Hitch.Report` | Frame-time buckets, and how much stall is still unattributed. |
| `FPM.Off`, `FPM.Hold`, `FPM.Release` | Release everything FPM holds, without unloading the mod. |

Levels are `0` for silent, `1` for on, and `2` for verbose. The default is `1`. Set `FPM.Diag 0` to
silence every channel at once.

**A silent channel never disables a fix.** It stops the printing only. A quiet log therefore means the
game is calm, not that the mod is absent. One exception is stated on purpose: each fix prints its
**arm line** whatever the level. That line is what separates "armed and saw nothing" from "never
armed".

The debug overlay is on by default while the mod is pre-release. Press **F8** to toggle it. Rebind it
with `FPM.Diag.OverlayKey`. The overlay exists so that evidence survives in a screenshot.

---

## Hard rules

Each rule is here because breaking it cost something real.

1. **Zero residue.** FPM writes to no `.ini` file, ever. Every console variable it sets is a hold, and
   it releases every hold when it unloads. Run `FPM.Residue` to audit that claim from inside the game.
   An uninstall must leave the game exactly as FPM found it.
2. **No network activity, ever.** No HTTP, no sockets, no telemetry, no analytics, no update check.
3. **FPM never destroys your items or your character.** Not to fix a bug, not to clean up state, and
   not behind a confirmation box.
4. **Composable native hooks only.** FPM uses `SUBSCRIBE_METHOD`, `SUBSCRIBE_METHOD_VIRTUAL` and
   `AccessTransformers.ini`. It never replaces a function, and it never edits SML, FactoryGame or the
   engine.
5. **Hooks never arm in the editor.** The gate is a runtime check inside the hook ledger, not an `#if`
   around the subscribe calls. The SML docs warn that an `#if` hides errors until a shipping build.
6. **Every hook goes through the ledger.** A hook that skips the ledger is a hook the inventory lies
   about, which is worse than no inventory.
7. **`Any` is the default side.** Only use `NeverOnDedicatedServer` when a whole subsystem is missing on
   a server, such as the renderer, the audio device, or input. Never use it because a fix feels like a
   client thing. Most of what FPM repairs is decided on the server.
8. **Authority is a per-call question.** Call `HasAuthority()` at the call site. Never infer authority
   from `GetPlayerController(0)`.
9. **Guard the bug and keep the feature.** Cancel a call only when nobody wants it. Where the original
   behavior can still work, repair the bad input and let vanilla run.
10. **A silent diagnostic must cost close to nothing.** An earlier attempt at schematic diagnostics
    wrote and flushed the log on every call, and it froze the game. `IsOn()` is one cached integer
    compare, and you test it before you build any string.
11. **FPM ships no game assets and no third-party binaries.** Original code only.
12. **A fix is not done when it compiles.** It is done when something calls `Arm()`, and a log line
    proves the hook installed.

---

## Build from source

Set up the Satisfactory C++ modding environment first. Follow the
[SML C++ setup guide](https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/index.html).
You need UE 5.6.1-CSS and Visual Studio 2022 with the C++ and game-development workloads. Check this
repo out at `SatisfactoryModLoader/Mods/GameFeatures/FicsitsPerformanceManager/`.

```bat
REM iterate on the code
Build.bat FactoryEditor Win64 Development -Project="<...>\FactoryGame.uproject" -Module=FicsitsPerformanceManager

REM build the binary the game actually loads
Build.bat FactoryGameSteam Win64 Shipping -Project="<...>\FactoryGame.uproject"

REM package all three targets
RunUAT.bat -ScriptsForProject="<...>\FactoryGame.uproject" PackagePlugin ^
    -project="<...>\FactoryGame.uproject" -DLCName=FicsitsPerformanceManager ^
    -clientconfig=Shipping -serverconfig=Shipping -Merge -utf8output
```

Three traps, each one paid for:

1. **Build the client Shipping target before you package.** `PackagePlugin` stages binaries from a
   build receipt. With no receipt it reports success and writes a payload that has no `Binaries`
   directory. That payload installs as a content-only mod, and every hook silently does not exist.
   Check the payload file list, not the exit code.
2. **The flag is `-DLCName`, not `-PluginName`.** UE ignores `-PluginName` without a word. The cook then
   degrades from a two-asset job into a full-game job, and it dies in an error that hides the real
   cause.
3. **Do not copy a locally built DLL into the game.** The `.modules` file carries a build id. SML
   packaging rewrites that id, and a local build stamps its own. The loader rejects the mismatch and
   reports that the module is missing, with the DLL sitting in the folder. Package first, then deploy
   the packaged payload.

---

## Transparency

- **License: [GPL-3.0](./LICENSE).** Every source file carries the header.
- **Network activity: none.** The mod is fully offline. This is hard rule 2. Grep `Source/` for HTTP,
  sockets, analytics or telemetry and you get zero hits. `tools/check_structure.py` also checks it.
- **AI-assisted, human-reviewed.** A human reviews every change before it lands. This disclosure is
  deliberate and it stays here.

## Bugs and contributions

Read [CONTRIBUTING.md](./CONTRIBUTING.md) first.

Report bugs at
[GitHub issues](https://github.com/DegradingAnt/Ficsit-performance-manager-/issues).

To report a bug, include four things:

1. The `[FPM]` log lines around the problem.
2. Whether you played single-player, listen host, or dedicated server.
3. The output of `FPM.Support`.
4. What you did just before it happened.

[CHANGELOG.md](./CHANGELOG.md) is append-only, newest first, one entry per change. Each entry states
what changed, why, which files, how to revert, and **how it was verified**.
