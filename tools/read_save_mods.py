#!/usr/bin/env python3
# WHY NOT RUST: same reason as the other tools beside it - this is the Satisfactory project, whose
# tools/ is Python by convention (check_mod_linkage.py, extract_scalability.py, package_fpm.py,
# deploy_fpm.py, pull_ficsit_guides.py are all Python here), and the job is byte-slicing plus
# json.JSONDecoder().raw_decode - no case for a compiled language. LAW 4: apply THIS project's rules;
# the Rust-everywhere law is vox-scoped by its own wording. Ant confirmed it directly on 2026-08-12:
# "yeah just use python here".
"""Read every mod SML recorded in a save's header - offline, with no game launch.

═══ WHY THIS EXISTS ═══

SML writes the full mod list AND EACH MOD'S VERSION into every save's header, uncompressed, before
the compressed world data starts. That gives two things nothing else does without booting the game:

  1. PROOF a boot test actually ran with FPM loaded, and at which version - the save itself is the
     receipt, not a screenshot or a remembered log line.
  2. A PARITY CHECK across Ant's client, SunFry's client, and the Linux dedicated server: three
     independent mod stacks that must agree, and a version drift between them is exactly the shape of
     bug that produces a "works for me" report.

═══ THE FORMAT, MEASURED 2026-08-14 ═══

Saves live under a numeric Steam-ID subfolder of SaveGames. Confirmed against a real save
(76561198105383027\\Transylvania_autosave_0.sav): 141 mods recorded, FicsitsPerformanceManager at
0.11.1, header JSON found at byte offset 13410 - well inside the first 400 KB this tool reads.

The header is ONE JSON object with exactly three keys - Version, Mods, FullMapName - sitting as plain
UTF-8 text near the start of the file; everything after it is compressed binary and this tool never
touches it. Mods entries look like {"Reference":..., "Name":..., "Version":...}. There is NO separate
GameFeatures section - GameFeature plugins, FPM included, appear in Mods like any other mod.

⚠ PARSING TRAP THAT COST TWO ATTEMPTS: do not regex for the closing brace. A pattern like
`\\{"Version":\\d+,"Mods":\\[.*?\\]\\}` OVER-CAPTURES past the real end of the object and json.loads
dies with "Extra data" - the header's length is not known ahead of time, so there is nothing to anchor
a regex to. json.JSONDecoder().raw_decode() has no such problem: it stops exactly where the object
ends and hands back that offset. So this reads the first slice of the file as text, finds the literal
`{"Version"`, and lets raw_decode find the close itself.

⚠ SEVERAL OLD SAVES HAVE NO HEADER JSON AT ALL. Measured against three 2024-era saves in the same
folder (a `.sav` with a blank Steam-cloud name, `1.0_110924-211154.sav`, `1.0_110924-211727.sav`): the
`{"Version"` marker is simply absent from the first 400 KB. That is counted and reported every run,
never silently skipped - an old save format is a different finding from "SML recorded nothing here",
and collapsing the two would hide which one actually happened.

    python tools/read_save_mods.py                          # scan every save, newest first
    python tools/read_save_mods.py --save PATH               # full mod list for one save
    python tools/read_save_mods.py --compare A.sav B.sav      # parity diff - the point of this tool

PATH and A/B may be a bare filename (searched under the SaveGames root) or a full path.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

# ⚠ FORCE UTF-8 ON STDOUT, OR THIS TOOL CAN CRASH EXACTLY WHEN IT HAS SOMETHING TO SAY. Mod names in
# a save's header are player/author-supplied text and can carry non-ASCII characters; a cp1252
# console dying on one mid-report loses everything printed after it while still exiting non-zero,
# which is indistinguishable from a real finding. See check_mod_linkage.py:57-68 for the incident
# this pattern is copied from.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# %LOCALAPPDATA%, not a hardcoded username - unlike the sibling tools' game-install paths, this one
# is per-Windows-user and hardcoding "linde" would silently break on any other machine.
SAVES_ROOT = Path(os.environ.get("LOCALAPPDATA", "")) / "FactoryGame" / "Saved" / "SaveGames"

HEADER_MARKER = '{"Version"'
HEADER_READ_BYTES = 400 * 1024  # measured: the real header sits at ~13 KB; this is generous headroom

FPM_REF = "FicsitsPerformanceManager"  # this mod's Reference key inside Mods[]
ABSENT = "ABSENT"


@dataclass
class SaveHeader:
    path: Path
    mtime: float
    status: str  # "ok" | "no_header" | "unreadable"
    version: int | None = None
    mods: list[dict] | None = None
    full_map: str | None = None
    error: str = ""


def read_header(path: Path) -> SaveHeader:
    """Read one save's header. Never raises - a bad save is a status, not a crashed run."""
    try:
        mtime = path.stat().st_mtime
    except OSError as e:
        return SaveHeader(path=path, mtime=0.0, status="unreadable", error=str(e))

    try:
        with path.open("rb") as f:
            raw = f.read(HEADER_READ_BYTES)
    except OSError as e:
        return SaveHeader(path=path, mtime=mtime, status="unreadable", error=str(e))

    text = raw.decode("utf-8", errors="replace")
    i = text.find(HEADER_MARKER)
    if i < 0:
        return SaveHeader(path=path, mtime=mtime, status="no_header")

    try:
        obj, _ = json.JSONDecoder().raw_decode(text[i:])
    except json.JSONDecodeError as e:
        return SaveHeader(path=path, mtime=mtime, status="no_header",
                           error=f"'{HEADER_MARKER}' marker found but did not parse: {e}")

    if not isinstance(obj, dict) or "Mods" not in obj:
        return SaveHeader(path=path, mtime=mtime, status="no_header",
                           error="parsed, but the object has no 'Mods' key")

    return SaveHeader(path=path, mtime=mtime, status="ok",
                       version=obj.get("Version"), mods=obj.get("Mods", []),
                       full_map=obj.get("FullMapName"))


def fpm_version(mods: list[dict]) -> str:
    for m in mods:
        if m.get("Reference") == FPM_REF:
            return str(m.get("Version", "?"))
    return ABSENT


def find_saves() -> list[Path]:
    return list(SAVES_ROOT.rglob("*.sav"))


def resolve_save_arg(arg: str) -> Path | None:
    """A full path if it exists, else the one save under SAVES_ROOT with that filename."""
    p = Path(arg)
    if p.is_file():
        return p
    if not SAVES_ROOT.is_dir():
        return None
    hits = list(SAVES_ROOT.rglob(p.name))
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        print(f"⚠ AMBIGUOUS: {len(hits)} saves named '{p.name}' under {SAVES_ROOT}:")
        for h in hits:
            print(f"    {h}")
    return None


def coverage_line() -> None:
    print()
    print("COVERAGE: this reports WHAT SML RECORDED AT SAVE TIME - it proves a mod was present, and")
    print("at what version, when the save was written. It does NOT prove the mod functioned, and it")
    print("cannot see content SML never registered in the first place; only the game's own log/crash")
    print("output can answer that. The compressed world data after the header is never read.")


def cmd_scan() -> int:
    if not SAVES_ROOT.is_dir():
        print(f"⚠ NOT CHECKED: save root does not exist: {SAVES_ROOT}")
        return 1

    saves = find_saves()
    if not saves:
        print(f"⚠ NOT CHECKED: no .sav files found under {SAVES_ROOT}")
        return 1

    results = [read_header(p) for p in saves]
    results.sort(key=lambda r: r.mtime, reverse=True)

    ok = [r for r in results if r.status == "ok"]
    no_header = [r for r in results if r.status == "no_header"]
    unreadable = [r for r in results if r.status == "unreadable"]

    name_w = max(len(r.path.name) for r in results)
    print(f"{'SAVE':<{name_w}}  {'MODIFIED':<19}  {'MODS':>5}  FPM VERSION")
    for r in results:
        ts = datetime.fromtimestamp(r.mtime).strftime("%Y-%m-%d %H:%M:%S") if r.mtime else "?"
        if r.status == "ok":
            print(f"{r.path.name:<{name_w}}  {ts:<19}  {len(r.mods):>5}  {fpm_version(r.mods)}")
        elif r.status == "no_header":
            print(f"{r.path.name:<{name_w}}  {ts:<19}  {'--':>5}  NO HEADER JSON")
        else:
            print(f"{r.path.name:<{name_w}}  {ts:<19}  {'--':>5}  UNREADABLE ({r.error})")

    print()
    print(f"{len(results)} save(s) under {SAVES_ROOT}: {len(ok)} with a readable header, "
          f"{len(no_header)} with NO header JSON (old save format or unparseable), "
          f"{len(unreadable)} unreadable (I/O error).")
    coverage_line()
    return 0


def cmd_save(arg: str) -> int:
    path = resolve_save_arg(arg)
    if path is None:
        print(f"⚠ NOT FOUND: '{arg}' - checked that path directly, then by filename under {SAVES_ROOT}")
        return 1

    r = read_header(path)
    if r.status != "ok":
        print(f"⚠ {r.status.upper()}: {path}")
        if r.error:
            print(f"  {r.error}")
        return 1

    print(f"{path}")
    print(f"SML save-format Version: {r.version}   FullMapName: {r.full_map}")
    print(f"{len(r.mods)} mod(s):\n")
    for m in sorted(r.mods, key=lambda m: (m.get("Reference") or "")):
        ref = m.get("Reference", "?")
        ver = m.get("Version", "?")
        name = m.get("Name", "")
        marker = "  <-- FPM" if ref == FPM_REF else ""
        print(f"  {ref:<40} {ver:<15} {name}{marker}")
    coverage_line()
    return 0


def cmd_compare(a_arg: str, b_arg: str) -> int:
    a_path = resolve_save_arg(a_arg)
    b_path = resolve_save_arg(b_arg)
    if a_path is None:
        print(f"⚠ NOT FOUND: A = '{a_arg}'")
    if b_path is None:
        print(f"⚠ NOT FOUND: B = '{b_arg}'")
    if a_path is None or b_path is None:
        return 1

    a = read_header(a_path)
    b = read_header(b_path)
    bad = False
    for label, r in (("A", a), ("B", b)):
        if r.status != "ok":
            print(f"⚠ {label} ({r.path}): {r.status.upper()}" + (f" - {r.error}" if r.error else ""))
            bad = True
    if bad:
        return 1

    a_mods = {(m.get("Reference") or "?"): m for m in a.mods}
    b_mods = {(m.get("Reference") or "?"): m for m in b.mods}

    only_a = sorted(set(a_mods) - set(b_mods))
    only_b = sorted(set(b_mods) - set(a_mods))
    shared = sorted(set(a_mods) & set(b_mods))
    mismatched = [(ref, a_mods[ref].get("Version"), b_mods[ref].get("Version"))
                  for ref in shared if a_mods[ref].get("Version") != b_mods[ref].get("Version")]

    print(f"A: {a.path}  ({len(a.mods)} mods)")
    print(f"B: {b.path}  ({len(b.mods)} mods)")
    print()

    if only_a:
        print(f"ONLY IN A ({len(only_a)}):")
        for ref in only_a:
            print(f"  {ref}  {a_mods[ref].get('Version', '?')}")
        print()
    if only_b:
        print(f"ONLY IN B ({len(only_b)}):")
        for ref in only_b:
            print(f"  {ref}  {b_mods[ref].get('Version', '?')}")
        print()

    if mismatched:
        print(f"★ {len(mismatched)} MOD(S) PRESENT IN BOTH, AT DIFFERENT VERSIONS - the parity answer:")
        for ref, va, vb in mismatched:
            print(f"  {ref:<40} A={va}   B={vb}")
        print()

    coverage_line()

    if only_a or only_b or mismatched:
        print(f"\nRESULT: NOT at parity - {len(only_a)} only-in-A, {len(only_b)} only-in-B, "
              f"{len(mismatched)} version mismatch(es).")
        return 1
    print("\nRESULT: identical mod sets, identical versions.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--save", metavar="PATH", help="dump the full mod list for one save")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"), help="diff two saves' mod sets")
    args = ap.parse_args()

    if args.compare:
        return cmd_compare(*args.compare)
    if args.save:
        return cmd_save(args.save)
    return cmd_scan()


if __name__ == "__main__":
    sys.exit(main())
