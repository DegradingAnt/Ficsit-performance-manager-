# Contributing to WNLPackFix

Thanks for your interest. WNLPackFix is maintained by **DegradingAnt (Ant)** under a benevolent-dictator model: Ant owns the project and has final say. Contributions are welcome as issues and pull requests.

> **AI-assisted, human-reviewed.** This project is developed with AI assistance and every change is human-reviewed before merge. That disclosure is intentional and stays in the README. If you use an AI assistant on a PR, that's fine — say so, and make sure you understand and have reviewed the code you submit.

## Before you start

- **Read the "Hard rules / architecture" section of the [README](./README.md).** Those are load-bearing invariants from real boot regressions (no sync occlusion CVars / no runtime Nanite buffer-cap raises = flashing; relief-only MaxOf/MinOf levers; graphics floor = the user's own settings; frame-gen never touched; composable hooks only; server/client self-guarding). A PR that violates one will be sent back regardless of how clean the code is.
- **Open an issue first for anything non-trivial.** Especially new CVar levers, new hooks, or behavior changes — Ant may already know why a lever was rejected in a prior boot-test.
- **Scope stays tight.** This mod is a perf/graphics governor + a small set of pack fixes. Feature creep (unrelated gameplay changes, new content) is out of scope.

## Development setup

You need the Satisfactory C++ modding environment per the [SML C++ setup guide](https://docs.ficsit.app/satisfactory-modding/latest/Development/Cpp/index.html):

- **UE 5.6.1-CSS** (the CSS-patched engine), Visual Studio 2022 with C++/game-dev workloads.
- The `SatisfactoryModLoader` starter project, with this repo checked out at `SatisfactoryModLoader/Mods/WNLPackFix/`.
- Target game build: **Satisfactory 1.2.3.1 (CL 495413)**, SML `^3.12.0`.

## Build

- **Iterate:** build the `FactoryGame` **Development Editor** target (in-editor), or **Shipping** for a runtime `.dll`.
- **CLI:** `Build.bat WNLPackFix` from the SML root, or a direct UBT invocation (see README → *Build from source*).
- **Package (all three targets):** the **Alpakit** *Package* button, or
  `RunUAT.bat PackagePlugin -Project="<...>/FactoryGame.uproject" -PluginName=WNLPackFix -Merge`.
  Windows **and** Linux server targets must both build — the mod has server-side hooks.

## Test / boot-test discipline

Compiling is **not** "done." This project has been bitten repeatedly by changes that built clean and then flickered or crashed in-world. Follow this discipline:

1. **One variable per boot.** Change one lever/hook, boot, observe, before stacking the next. A boot that changes three things can't attribute the regression.
2. **Boot-test both sides when a change touches server behavior.** The RPC gate, static-base fix, navmesh fix, and Wwise gate are server-authoritative — verify on a **dedicated server + a joined client**, not just single-player. Client-only changes (governor, fog, contact shadows) can be verified on a listen/SP session.
3. **Watch the log for the arm lines.** Every fix logs a `[WNLPackFix]` arm line on startup and heartbeats while working (e.g. suppressed-dispatch counts, stage transitions, `raised N nav classes`). Confirm the feature you touched actually armed.
4. **Verify visual levers by eye, over time.** Flashing/shimmer regressions often only show under motion, at the resolution floor, or during the join/PSO-compile storm — not on a static first frame. Move around, load into a busy factory, watch foliage and belt items.
5. **Bump the version string** in `WNLPackFix.uplugin` (`SemVersion` == `VersionName`, `Version` integer = first digit of SemVersion) and the `StartupModule` log line so the running build is traceable from the log.
6. **Confirm no new network activity.** The mod must stay fully offline (only the local Windows DXGI VRAM read). Don't add HTTP/sockets/telemetry.
7. **Prefer `WNLPackFix.Status`** in-game to confirm live governor state (vendor, active upscaler, res %, stage, CPU-relief intensity) matches what your change intended.

## Pull requests

- Keep PRs focused; one logical change per PR.
- Explain the **why** and the **boundary** (what it does *not* touch) — the codebase documents boundaries heavily; match that style.
- Note how you boot-tested it (which target, SP vs dedicated+client, what you observed).
- Comment new CVar levers with *why they're free / when they cost ms*, and clamp any new config key on load (a hand-edited config must never divide by zero, invert a band, or defeat a hysteresis gate).
- By submitting a PR you agree your contribution is licensed under the project's `LICENSE`.

## Reporting bugs

Open an issue with: your GPU vendor + active upscaler, whether it's SP / listen / dedicated, the `[WNLPackFix]` log lines around the problem, and repro steps. For visual issues, a short clip beats a screenshot (flashing doesn't show in a still).
