#!/usr/bin/env python3
# WHY NOT RUST: same reason as package_fpm.py beside it — this is the Satisfactory project, whose
# tools/ is Python, and the job is unzip-plus-filesystem work where Rust buys nothing. LAW 4: apply
# THIS project's rules, and the Rust-everywhere law is vox-scoped by its own wording.
"""Deploy a packaged FPM payload to the game, by REPLACING the folder rather than writing over it.

═══ WHY THIS EXISTS ═══

Ant, 2026-08-11: *"Remember that you need to do clean updates for every update. Not just overright but
remove the old ones before putting in the new stuff"*.

She said it because I had just done the wrong thing. Deploying 0.11.25 by extracting the zip OVER the
live 0.11.13 folder left behind:

  · two 70 MB PDBs from the previous package, which the new symbol-free package could not overwrite;
  · every file the old version shipped that the new one no longer does — invisible, because an
    extract only ever ADDS and REPLACES, never removes.

⚠ THAT IS NOT MERELY UNTIDY. The first boot after that deploy died on

    Assertion failed: NumRemoved==1
    [File: .../CoreUObject/Private/UObject/UObjectArray.cpp] [Line: 436]

which is the engine finding a UObject registration it cannot cleanly remove — exactly the shape a
stale binary sitting beside a current one can produce. Whether that WAS the cause is unproven, and
this script is not offered as the fix for it; it removes the whole class of question.

★ THE RULE: A DEPLOY IS A REPLACE, NOT A MERGE. If the new payload does not contain a file, that file
must not survive the deploy.

═══ WHAT IT PROTECTS ═══

⚠ `DerivedBoxes.json` is FPM's own cache and lives in the deployed folder. It is expensive to rebuild
(a full class sweep at world load) and is NOT part of the package, so a naive wipe would silently cost
the next boot a long stall. It is preserved across the replace, deliberately and visibly.

  python tools/deploy_fpm.py               # replace the client install with the packaged Windows payload
  python tools/deploy_fpm.py --dry-run     # say exactly what would be removed and added, change nothing

⚠ IT DOES NOT TOUCH THE DEDICATED SERVER. That needs the server STOPPED and is Ant's call every time.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import zipfile
from pathlib import Path

PLUGIN_DIR = Path(__file__).resolve().parent.parent
SML_ROOT = PLUGIN_DIR.parents[2]
ARCHIVE = SML_ROOT / "Saved" / "ArchivedPlugins" / "FicsitsPerformanceManager" / "FicsitsPerformanceManager-Windows.zip"
GAME_MOD_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Satisfactory\FactoryGame\Mods\GameFeatures\FicsitsPerformanceManager")

# Files that live in the deployed folder, are NOT in the package, and must survive a replace.
# Keep this list short and justified — every entry is something a wipe would silently destroy.
PRESERVE = {
    "DerivedBoxes.json",   # FPM's own bounds cache; rebuilding it costs a world-load sweep
}


def fail(msg: str) -> None:
    print(f"FAIL  {msg}")


def ok(msg: str) -> None:
    print(f"ok    {msg}")


def version_in(path_or_bytes) -> str | None:
    raw = path_or_bytes if isinstance(path_or_bytes, bytes) else Path(path_or_bytes).read_bytes()
    for enc in ("utf-8-sig", "utf-16", "utf-16-le"):
        try:
            m = re.search(r'"VersionName"\s*:\s*"([^"]+)"', raw.decode(enc))
            if m:
                return m.group(1)
        except UnicodeDecodeError:
            continue
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="report what would change; write nothing")
    args = ap.parse_args()

    if not ARCHIVE.exists():
        fail(f"no packaged payload at {ARCHIVE} — run tools/package_fpm.py first")
        return 1

    with zipfile.ZipFile(ARCHIVE) as z:
        names = z.namelist()
        upl = next((n for n in names if n.endswith(".uplugin")), None)
        new_version = version_in(z.read(upl)) if upl else None

    if new_version is None:
        fail("could not read a VersionName out of the packaged payload — refusing to deploy it")
        return 1

    old_version = None
    live_uplugin = GAME_MOD_DIR / "FicsitsPerformanceManager.uplugin"
    if live_uplugin.exists():
        old_version = version_in(live_uplugin)

    print(f"deploying {new_version}  (currently installed: {old_version or '<nothing>'})")
    print(f"  from {ARCHIVE}")
    print(f"  to   {GAME_MOD_DIR}")

    # What the replace would REMOVE that the new payload does not restore. This is the whole point of
    # the script, so it is printed every time rather than only on --dry-run.
    if GAME_MOD_DIR.exists():
        incoming = {n.replace("/", "\\") for n in names if not n.endswith("/")}
        existing = {
            str(p.relative_to(GAME_MOD_DIR)) for p in GAME_MOD_DIR.rglob("*") if p.is_file()
        }
        orphans = sorted(existing - incoming - PRESERVE)
        if orphans:
            total = sum((GAME_MOD_DIR / o).stat().st_size for o in orphans)
            print(f"\n  {len(orphans)} file(s) in the install are NOT in the new payload "
                  f"({total:,} bytes). A replace removes them; an extract would have left them:")
            for o in orphans[:12]:
                print(f"    - {o}  ({(GAME_MOD_DIR / o).stat().st_size:,} bytes)")
            if len(orphans) > 12:
                print(f"    ... and {len(orphans) - 12} more")
        else:
            print("\n  no orphaned files — the install already matches the payload's shape")

    if args.dry_run:
        print("\n--dry-run: nothing written.")
        return 0

    # Preserve first, wipe, extract, restore.
    stash: dict[str, bytes] = {}
    for name in PRESERVE:
        p = GAME_MOD_DIR / name
        if p.exists():
            stash[name] = p.read_bytes()
            ok(f"preserved {name} ({len(stash[name]):,} bytes) across the replace")

    if GAME_MOD_DIR.exists():
        shutil.rmtree(GAME_MOD_DIR)
        ok("removed the old install")

    GAME_MOD_DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ARCHIVE) as z:
        z.extractall(GAME_MOD_DIR)
    ok(f"extracted {len(names)} entries")

    for name, data in stash.items():
        (GAME_MOD_DIR / name).write_bytes(data)
        ok(f"restored {name}")

    # ── VERIFY THE ARTEFACT, not the step (LAW 7) ────────────────────────────────────────────────
    good = True
    deployed = version_in(live_uplugin) if live_uplugin.exists() else None
    if deployed != new_version:
        fail(f"deployed .uplugin reads {deployed!r}, expected {new_version!r}")
        good = False
    else:
        ok(f"deployed version reads {deployed}")

    bins = list((GAME_MOD_DIR / "Binaries").rglob("*.dll")) if (GAME_MOD_DIR / "Binaries").is_dir() else []
    if not bins:
        fail("no DLLs in the deployed Binaries/ — this install is CONTENT-ONLY and every hook is absent")
        good = False
    else:
        ok(f"{len(bins)} DLL(s) present")

    strays = [p for p in GAME_MOD_DIR.rglob("*.pdb")]
    if strays:
        fail(f"{len(strays)} PDB(s) still present after a clean replace — the payload should carry none")
        good = False
    else:
        ok("no debug symbols in the install")

    if not good:
        print("\nRESULT: the deploy is NOT sound. See the FAIL lines above.")
        return 1

    print(f"\nRESULT: {new_version} deployed clean. Old install removed, not written over.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
