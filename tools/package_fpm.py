#!/usr/bin/env python3
# WHY NOT RUST: this is the Satisfactory project, not VOX — the Rust-everywhere law is vox-scoped by
# its own wording (LAW 4: apply THIS project's rules). FPM's tools/ is already Python (check_structure.py,
# and the sibling server tools), this script is invoked by the same hands and CI paths as those, and its
# entire job is shelling out to RunUAT/Build.bat and reading a zip's central directory — work where Rust
# buys nothing and a lone cargo project inside a Python tools/ folder costs the next maintainer real time.
"""Package FPM for every shipping target, then PROVE the archives are what they claim to be.

═══ WHY THIS EXISTS ═══

`RunUAT PackagePlugin` returns exit 0 while producing an archive that is wrong in ways nobody sees.
Measured on 2026-08-11, all three faults in a single run:

  1. NO SERVER PAYLOAD AT ALL. The invocation used `-serverconfig=Shipping`, which sets the server
     CONFIGURATION and nothing else. PackagePlugin.cs:36 gates the server on
     `projectParams.DedicatedServer`, and ProjectParams.cs:863 sets that from `-dedicatedserver` or
     `-server` — neither of which was passed. The merged zip contained exactly one top-level folder,
     `Windows`, and the two server zips on disk were two hours stale from an earlier run.
     Exit code: 0.
  2. 98.8% OF THE PAYLOAD WAS DEBUG SYMBOLS. Two PDBs at 70,250,496 and 70,316,032 bytes against an
     879,616-byte DLL. `ArchiveStagedPlugin` (PackagePlugin.cs:173) zips the staged directory
     wholesale with no exclusions, so anything staged ships. `-nodebuginfo` keeps them out of the
     stage in the first place (ProjectParams.cs:2122-2125).
  3. A STALE ZIP IS INDISTINGUISHABLE FROM A FRESH ONE unless you look at mtimes. Three of the four
     archives on disk were from a previous version and looked exactly like output.

Every one of those passes a "did the command succeed" check. None passes an "is the artefact right"
check. LAW 7: verify the ARTEFACT, not the step that produced it.

═══ WHAT THIS IS ═══

A GATE, not a reporter. It exits non-zero when the payload is wrong, so it cannot be read past.
(sf-toolfix: "A reporter exits 0 and classifies; a gate exits non-zero. Never both.")

  python tools/package_fpm.py                  # build targets, package, verify
  python tools/package_fpm.py --verify         # verify existing archives only, build nothing
  python tools/package_fpm.py --keep-symbols   # ship PDBs on purpose (a private debug build)

⚠ IT DOES NOT DEPLOY. Deploying to the dedicated server needs the server STOPPED, and that is Ant's
call every time, never a script's.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
import zipfile
from pathlib import Path

# ── Paths. Absolute and derived, never relative to the caller's cwd (sf-toolfix: a tool called from
#    the wrong directory produces a confident wrong answer). ─────────────────────────────────────────
PLUGIN_DIR = Path(__file__).resolve().parent.parent
SML_ROOT = PLUGIN_DIR.parents[2]            # .../Mods/GameFeatures/<plugin> -> SML root
UPROJECT = SML_ROOT / "FactoryGame.uproject"
UPLUGIN = PLUGIN_DIR / "FicsitsPerformanceManager.uplugin"
ARCHIVE_DIR = SML_ROOT / "Saved" / "ArchivedPlugins" / "FicsitsPerformanceManager"
ENGINE = Path(r"C:\Program Files\Unreal Engine - CSS\Engine")
BUILD_BAT = ENGINE / "Build" / "BatchFiles" / "Build.bat"
RUNUAT_BAT = ENGINE / "Build" / "BatchFiles" / "RunUAT.bat"

DLC_NAME = "FicsitsPerformanceManager"

# ── The target matrix. A SET you cannot partially satisfy — sf-ship's rule, learned when "the Linux
#    target compiles" was recorded as progress and no server package existed for six versions. ──────
TARGETS = [
    ("FactoryGameSteam", "Win64", "Shipping"),
    ("FactoryServer", "Win64", "Shipping"),
    ("FactoryServer", "Linux", "Shipping"),
]

# Top-level folders the MERGED archive must contain. These are DeploymentContext.FinalCookPlatform
# values (PackagePlugin.cs:204), not target names.
REQUIRED_MERGED_FOLDERS = {"Windows", "WindowsServer", "LinuxServer"}

REQUIRED_PER_TARGET_ZIPS = [
    f"{DLC_NAME}-Windows.zip",
    f"{DLC_NAME}-WindowsServer.zip",
    f"{DLC_NAME}-LinuxServer.zip",
]

# An archive older than this when the run finishes is a LEFTOVER, not output.
FRESH_WINDOW_SEC = 3600


def fail(msg: str) -> None:
    print(f"FAIL  {msg}")


def ok(msg: str) -> None:
    print(f"ok    {msg}")


def source_version() -> str:
    """Read VersionName out of the SOURCE .uplugin. Everything else is compared against this."""
    data = json.loads(UPLUGIN.read_text(encoding="utf-8-sig"))
    return data["VersionName"]


def run(cmd: list[str], what: str) -> bool:
    print(f"\n>>> {what}")
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    tail = [l for l in (proc.stdout or "").splitlines() if re.search(r"error|Result:|BUILD ", l, re.I)]
    for line in tail[-12:]:
        print(f"    {line}")
    if proc.returncode != 0:
        fail(f"{what} exited {proc.returncode}")
        return False
    return True


def build_targets() -> bool:
    """Build every shipping target BEFORE packaging.

    ⚠ THIS ORDER IS LOAD-BEARING AND IS ITS OWN RECORDED FAILURE. PackagePlugin stages binaries from
    a build RECEIPT. With no receipt for a target it stages nothing for it and still reports success —
    the 2026-08-08 case produced a 76,625-byte archive of which 76,548 bytes was the icon, and the mod
    then loaded as content-only with every hook silently absent.
    """
    all_ok = True
    for target, platform, config in TARGETS:
        cmd = [str(BUILD_BAT), target, platform, config, f"-Project={UPROJECT}"]
        if not run(cmd, f"build {target} {platform} {config}"):
            all_ok = False
    return all_ok


def package(keep_symbols: bool) -> bool:
    """The one correct invocation, baked in so it cannot be typed wrong again.

    Each flag below cost something to learn; the comments are the receipts.
    """
    cmd = [
        str(RUNUAT_BAT),
        # MUST COME FIRST and name the PROJECT: PackagePlugin is Alpakit's command, living in
        # Mods/Alpakit/Source/Alpakit.Automation/, not the engine's. Without this UAT reports
        # "Failed to find command PackagePlugin", which reads as "this engine cannot package".
        f"-ScriptsForProject={UPROJECT}",
        "PackagePlugin",
        f"-project={UPROJECT}",
        # -DLCName, NOT -PluginName. PackagePlugin.cs reads ProjectParams.DLCFile; -PluginName is
        # silently ignored and degrades the DLC cook into a 45-minute full-game cook that then dies
        # in a NullReferenceException hiding the real cause.
        f"-DLCName={DLC_NAME}",
        "-clientconfig=Shipping",
        # ★ -server IS WHAT TURNS THE SERVER ON. -serverconfig alone sets a configuration for a
        # target that never gets created (PackagePlugin.cs:36 -> ProjectParams.cs:863).
        "-server",
        "-serverconfig=Shipping",
        "-ServerTargetPlatform=Win64+Linux",
        # Produce the merged archive as well as the per-target ones.
        "-merge",
        "-utf8output",
    ]
    if not keep_symbols:
        # ProjectParams.cs:2122-2125 "do not copy debug files to the stage". Keeping them out of the
        # STAGE is the only lever that works: ArchiveStagedPlugin zips whatever it finds there.
        cmd.append("-nodebuginfo")
    return run(cmd, "package plugin (all targets)")


def verify(expect_version: str, keep_symbols: bool, cutoff: float) -> bool:
    """Prove the archives are current, complete, and carry the version we think they do."""
    print("\n>>> VERIFY THE ARTEFACT")
    good = True

    if not ARCHIVE_DIR.is_dir():
        fail(f"no archive directory at {ARCHIVE_DIR}")
        return False

    # 1. Every per-target zip exists AND is from THIS run.
    for name in REQUIRED_PER_TARGET_ZIPS:
        p = ARCHIVE_DIR / name
        if not p.exists():
            fail(f"{name} MISSING — that target did not package")
            good = False
            continue
        age = cutoff - p.stat().st_mtime
        if age > FRESH_WINDOW_SEC:
            fail(f"{name} is STALE ({age/60:.0f} min old) — a leftover, not output from this run")
            good = False
        else:
            ok(f"{name}  {p.stat().st_size:,} bytes, fresh")

    # 2. The merged archive carries every platform, and the right version.
    merged = ARCHIVE_DIR / f"{DLC_NAME}.zip"
    if not merged.exists():
        fail(f"{merged.name} MISSING")
        return False

    with zipfile.ZipFile(merged) as z:
        names = z.namelist()
        folders = {n.split("/")[0] for n in names if "/" in n}
        missing = REQUIRED_MERGED_FOLDERS - folders
        if missing:
            fail(f"{merged.name} missing platform folder(s) {sorted(missing)} — found {sorted(folders)}")
            good = False
        else:
            ok(f"{merged.name} carries {sorted(folders)}")

        # 3. Binaries present per platform. A codeless payload is the 2026-08-08 failure and it looks
        #    identical to a good one from the outside.
        for folder in sorted(folders):
            bins = [n for n in names if n.startswith(f"{folder}/Binaries/")
                    and n.endswith((".dll", ".so", ".modules"))]
            if not bins:
                fail(f"{folder}/ has NO binaries — CONTENT-ONLY payload, every hook silently absent")
                good = False
            else:
                ok(f"{folder}/ has {len(bins)} binary file(s)")

        # 4. The version IN THE SHIPPED BYTES, not the source we hoped it built from.
        for n in [x for x in names if x.endswith(".uplugin")]:
            raw = z.read(n)
            got = None
            for enc in ("utf-8-sig", "utf-16", "utf-16-le"):
                try:
                    m = re.search(r'"VersionName"\s*:\s*"([^"]+)"', raw.decode(enc))
                    if m:
                        got = m.group(1)
                        break
                except UnicodeDecodeError:
                    continue
            if got != expect_version:
                fail(f"{n} says version {got!r}, source says {expect_version!r}")
                good = False
            else:
                ok(f"{n} -> {got}")

        # 6. DEV-ONLY DIRECTORIES MUST NOT SHIP. Review 2026-08-15, M4: ArtSource/ holds 52 MB of
        #    renders, .blend and .glb source art in a repo whose origin is D:, and NOTHING in the tree
        #    named it, so whether it reached the shipped zip was UNVERIFIED rather than known.
        #
        #    IT DOES NOT: PackagePlugin zips the STAGE directory, not the source tree
        #    (PackagePlugin.cs:173 ArchiveStagedPlugin -> CreateZipFromDirectory), and staging only takes
        #    the .uplugin, cooked Content, Binaries, Resources, plus anything Config/FilterPlugin.ini
        #    adds — and FPM ships no FilterPlugin.ini. Measured on the four archives present on
        #    2026-08-15: every per-target zip's top level is exactly Binaries, Content, Resources and
        #    FicsitsPerformanceManager.uplugin, with zero matches for any name below.
        #
        #    THIS CHECK IS WHY THAT STAYS TRUE. A FilterPlugin.ini added later for one legitimate extra
        #    file is one glob away from sweeping a 52 MB art tree into a mod release, and the zip would
        #    still look fine from the outside. Verify the ARTEFACT, not the step that produced it.
        dev_only = ("artsource/", "_dev/", "docs/", "tools/", ".git", "intermediate/", "saved/")
        shipped_dev = sorted({n for n in names
                              if any(part in n.lower() for part in dev_only)})
        if shipped_dev:
            fail(f"{len(shipped_dev)} DEV-ONLY path(s) in the shipped zip, e.g. {shipped_dev[:5]} — "
                 f"a release must carry Binaries, Content, Resources and the .uplugin, nothing else")
            good = False
        else:
            ok(f"no dev-only paths in {len(names)} shipped entries (checked {list(dev_only)})")

        # 5. Symbols. Must never be silent: shipping a 140 MB payload for an 880 KB mod is the kind of
        #    thing nobody notices until a user does.
        pdbs = [n for n in names if n.lower().endswith((".pdb", ".debug"))]
        total = sum(z.getinfo(n).file_size for n in names) or 1
        if pdbs:
            sym = sum(z.getinfo(n).file_size for n in pdbs)
            msg = f"{len(pdbs)} debug symbol file(s), {sym:,} of {total:,} bytes ({100*sym/total:.1f}%)"
            if keep_symbols:
                ok(f"{msg} — kept on purpose (--keep-symbols)")
            else:
                fail(f"{msg} — -nodebuginfo did not take effect")
                good = False
        else:
            ok(f"no debug symbols; payload is {total:,} bytes uncompressed")

    return good


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify", action="store_true",
                    help="verify existing archives only; build and package nothing")
    ap.add_argument("--keep-symbols", action="store_true",
                    help="ship PDBs on purpose (private debug build)")
    args = ap.parse_args()

    version = source_version()
    print(f"FPM package — source version {version}")
    print(f"  plugin  {PLUGIN_DIR}")
    print(f"  archive {ARCHIVE_DIR}")

    if not args.verify:
        if not BUILD_BAT.exists() or not RUNUAT_BAT.exists():
            fail(f"engine batch files not found under {ENGINE}")
            return 1
        if not build_targets():
            fail("a target failed to build — NOT packaging, because PackagePlugin would happily "
                 "produce an archive missing that target and report success")
            return 1
        if not package(args.keep_symbols):
            return 1

    if not verify(version, args.keep_symbols, time.time()):
        print("\nRESULT: the package is NOT shippable. See the FAIL lines above.")
        return 1

    print(f"\nRESULT: {DLC_NAME} {version} packaged and verified for {sorted(REQUIRED_MERGED_FOLDERS)}.")
    print("        NOT deployed. The dedicated server must be STOPPED first, and that is Ant's call.")
    # Say what 'verified' does NOT mean, beside the line that says it. See the memory
    # an-instrument-must-print-its-own-coverage. The checks above are strong on SHAPE - every target
    # present, fresh not stale, every platform folder, binaries not a codeless payload - and say
    # nothing about CONTENT.
    print("        NOT CHECKED: that the binaries contain THIS source. Freshness is a timestamp, not")
    print("        a diff - a zip written this run can still hold a DLL built before the last edit.")
    print("        The BuildId inside .modules is unread, and nothing here proves a hook arms.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
