# Contributing to Ficsit's Performance Manager

**DegradingAnt (Ant)** maintains this project and has the final say on every decision. Issues and pull
requests are welcome.

> **AI-assisted, human-reviewed.** A human reviews every change before it lands. That disclosure is
> deliberate and it stays in the README. You may use an AI assistant on a pull request. Say so, and
> make sure you understand and have reviewed the code you send.

## Before you start

1. Read the [hard rules](./README.md#hard-rules). They come from real regressions, not from taste. A
   pull request that breaks one goes back, however clean the code is.
2. Open an issue first for anything that is not trivial, such as a new hook, a new lever, or a change
   in behavior. The reason an approach was rejected often sits in an earlier boot test.
3. Keep the scope tight. This mod repairs, guards and measures real observed bugs. It does not add
   gameplay or content.

## Set up

You need UE 5.6.1-CSS and Visual Studio 2022 with the C++ and game-development workloads. Get the
`SatisfactoryModLoader` starter project. Check this repo out at
`SatisfactoryModLoader/Mods/GameFeatures/FicsitsPerformanceManager/`.

The target is Satisfactory **1.2.3.1 (CL 495413)** with SML **`^3.12.0`**. Build commands are in the
[README](./README.md#build-from-source).

## Write a fix

1. Put one fix in one class in one file. Implement `IFPMFix`. Use `Fixes/Vanilla/` when the bug is
   vanilla, and `Fixes/Interop/` when it arrives from another mod.
2. Answer all four contract members. `Name()`, `Side()`, `OriginStatus()` and `Channel()` are pure
   virtual, so a new fix cannot inherit somebody else's answer. Read the enum comments before you
   choose. `Any` is the default side, and the bar to leave it is high.
3. Never guess a function signature. Read the real header. A wrong guess either fails to compile, which
   is cheap, or binds the wrong overload in silence, which is not.
4. Check that the directory exists before you trust a grep that found nothing. A grep against a wrong
   path returns no output and exit code 0. That looks exactly like "the function is not in the header".
   The headers are at `<SML root>/Source/FactoryGame/Public/` and
   `C:/Program Files/Unreal Engine - CSS/Engine/Source/`.
5. Check that the hook target can be hooked. `SUBSCRIBE_METHOD_VIRTUAL` works on a virtual function
   only. funchook also refuses a target when the prologue is too short, when it uses IP-relative
   addressing, or when it contains a back jump. Two of those three do not depend on size, so "it is a
   big function" is not the whole check. If a hook is refused, read which error came back.
6. Use the `FPM_SUBSCRIBE` macros. Never call a raw `SUBSCRIBE_` macro.
7. Remember that this module is a unity build. The compiler joins the `.cpp` files into one translation
   unit, so a file-local constant is not file-local. Two anonymous namespaces that declare the same name
   are a redefinition error. Put shared constants in a header. Give a per-fix constant a name that is
   unique in the whole module.
8. Watch the arm order when two fixes share a hook target. `TCallScope::Override` stops the chain, so a
   fix that overrides silences every handler that registered after it. If that matters, say so at the
   `Arm()` call site. Do not leave it to be found again later.

## Verify before you claim

A clean compile is not "done". This project has been caught many times by a change that built clean and
then misbehaved in the game.

1. Compile first. A compile error is nearly free. A boot is not.
2. Read the signature from the header, not from a sibling file that already uses it. Copying a
   descriptor from another file is still copying it from memory.
3. Change one variable per boot. If two changes could each explain the result, the boot proves nothing
   about either one. Batch independent questions. Serialize confounded ones.
4. Make the answer visible before you launch. Check that the arm lines print, that the channel is at the
   right level, and that the overlay is on when the evidence has to survive in a screenshot.
5. Do not expect `Display` level `UE_LOG` to reach the in-game console. It does not. When a person has
   to read a value in the game, use an output-device command.
6. Boot-test both sides for anything the server decides. That means a dedicated server and a joined
   client, not single-player alone.
7. Treat a requested value as unverified until you read it back.
8. Say which of these steps you actually did. `build-only` is an honest answer and it is accepted. A
   claim of `boot-tested` that did not happen is not.

## Paperwork for every change

- **Add a [CHANGELOG.md](./CHANGELOG.md) entry at the top**, in the documented format: what, why,
  files, revert, and verified. Write one entry per change. Do not batch them, and do not leave them
  until release. Batched notes get guessed after the fact.
- **Bump the version** in `FicsitsPerformanceManager.uplugin` when the change ships. `VersionName` and
  `SemVersion` are always equal. `Version` is the first digit of `SemVersion`. The runtime carries no
  version literal. `StartupModule` reads the version from the loaded descriptor, so there is no second
  place to bump and no way for the log line to disagree with the file.
- **Choose MINOR or PATCH with one question: would a player notice this while playing, with the console
  closed?** If no, it is a PATCH, however new it is. Four brand-new console commands are new and still
  invisible.
- **Write comments that state the boundary.** Say what the change touches, and say what it deliberately
  does not touch. Match the density of the code around you. This codebase documents boundaries on
  purpose.
- **Treat a comment as a claim.** When you change code, read the comment above it against what the code
  now does. A stale comment that promises too much is worse than no comment, because it stops the next
  reader from looking.

## Pull requests

1. Send one logical change per pull request.
2. Explain the reason and the boundary.
3. Name the evidence: the log line, the header line, the measurement. "It seemed like it should be
   faster" is not evidence.
4. By sending a pull request you agree to license your work under [GPL-3.0](./LICENSE).

## Check the repository structure

`tools/check_structure.py` checks the layout and the metadata. It verifies that the license, README,
changelog and contributing files exist. It verifies the `.uplugin` fields against each other and against
the tested game build. It verifies that every fix header declares all four contract members, and that
every fix is armed.

```bash
python tools/check_structure.py
```

Run it before you open a pull request, and again before you package.

It exists for one reason. The first rewrite of this mod dropped its README, its LICENSE and its
CONTRIBUTING file, and it dropped three working fixes with them. A written checklist already covered all
of it. A checklist that nobody runs is how the same mistake happens twice.

## Release checklist

Do these before an upload to ficsit.app:

1. Run `python tools/check_structure.py` and get zero errors.
2. Build all three targets: Windows client, Windows server, Linux server.
3. Boot-test a dedicated server and a joined client on the packaged build.
4. Confirm that the payload contains a `Binaries` directory.
5. Answer the generative-AI disclosure field on the upload form. The site blocks the upload until you do.
6. Answer the network-activity field. The answer is "no network activity".
7. Set the compatibility state for both the Stable branch and the Experimental branch.
8. Check that the mod page states how to report a bug, and how to contact the author.
