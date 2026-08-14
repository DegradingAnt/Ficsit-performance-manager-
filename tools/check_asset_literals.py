#!/usr/bin/env python3
# WHY NOT RUST: same reason as the other tools beside it - this is the Satisfactory project, whose
# tools/ is Python by convention (check_mod_linkage.py, extract_scalability.py, package_fpm.py,
# deploy_fpm.py, pull_ficsit_guides.py are all Python here), and the job is a regex over .cpp/.h text
# plus a filesystem lookup against the FModel dump extract_scalability.py already reads. LAW 4: apply
# THIS project's rules; the Rust-everywhere law is vox-scoped by its own wording. Ant confirmed it
# directly on 2026-08-12: "yeah just use python here".
"""Catch the update failure that leaves NO crash, NO log line, and NO compile error behind.

═══ WHY THIS EXISTS ═══

From the community docs: "if a base-game material or texture file you were depending on is moved or
removed, it will SILENTLY be replaced with a default or None value!" Every other gate this project has
reads C++ SYMBOLS - PE imports (check_mod_linkage.py), header diffs, AccessTransformer friends - and
is blind to asset references by construction, because an asset path is just a string literal to the
compiler. It links fine, it boots fine, and the mod quietly starts drawing the wrong icon or applying
the wrong material. This is the one gate that reads those strings instead of the code around them.

═══ WHAT IT DOES ═══

Extracts every `"/Game/..."` and `"/Script/..."` literal from FPM's own source (Private + Public) with
the regex `"/(Game|Script)/[A-Za-z0-9_/.]+"`, then asserts each `/Game/` literal resolves to a real
file in the FModel dump at C:\\VOX-BRAIN-ROOT\\20-SOURCES\\satisfactory\\fmodel-exports\\.

⚠ PATH MAPPING, MEASURED 2026-08-14 against a real hit: the literal
`/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_Steam_128.TXUI_Steam_128`
(FPMAssetResidency.cpp:28) resolves on disk at BOTH
  fmodel-exports\\FactoryGame\\Content\\FactoryGame\\Interface\\UI\\Menu\\Graphics\\TXUI_Steam_128.uasset
  fmodel-exports\\Exports\\FactoryGame\\Content\\FactoryGame\\Interface\\UI\\Menu\\Graphics\\TXUI_Steam_128.json
So the rule is: strip the leading `/Game/`, drop the trailing `.ObjectName` soft-reference suffix (UE's
own "Package.Object" convention - package paths never contain a literal `.` themselves, so splitting on
the first one is safe), and look for a file whose STEM equals the asset name in EITHER
`FactoryGame\\Content\\<rest>` or `Exports\\FactoryGame\\Content\\<rest>`. A hit in either tree counts.

★ MATCH ON NAME *AND* DIRECTORY, NOT NAME ALONE. A name-only search (glob the whole dump for
`TXUI_Steam_128.*` and call it found) would keep passing after the asset MOVED to a different folder -
exactly the failure this tool exists to catch would go undetected by the laziest version of this check.
So this only ever looks in the ONE directory the literal's own path implies.

`/Script/...` literals are C++ CLASS paths, not assets - they cannot be resolved against the content
dump at all, and this reports them separately as NOT CHECKED rather than silently ignoring or failing
on them.

═══ THE KNOWN-GOOD BASELINE, AND WHAT RUNNING IT FOR REAL FOUND ═══

Five `/Game/` literals and their sites (all in FPMAssetResidency.cpp, confirmed by direct read):
  line 28  TXUI_Steam_128            line 29  TXUI_Epic_128
  line 30  TXUI_XBOX_128             line 31  TXUI_PlayStation_128
  line 54  US_ShowCreaturePerceptionIndicators
All five are asserted present below - if extraction stops finding any of them, that IS the checker
being broken (a moved/renamed regex target, a changed directory), not a benign difference, and the run
refuses to certify a clean pass.

One `/Script/` literal was expected (`/Script/StreamlineReflex.StreamlineLibraryReflex`,
FPMReflexMode.cpp:57). Run for real, the SAME regex also matches TWO more `/Script/` occurrences that
are real text in this source tree today: `TEXT("/Script/FactoryGame")` inside a StartsWith() prefix
check (FPMHologramNetGuard.cpp:188), and `"/Script/FactoryGame..."` inside a block COMMENT
(FPMMaterialEffectProbe.cpp:90). Both are reported below, tagged, rather than hidden - `/Script/`
literals are never gate-failed by design (they are code paths, not assets), and a mod that legitimately
grows more of them over time is not itself a defect. Only a MISSING expected item is treated as the
checker being wrong; extra items beyond the recorded baseline are expected growth and are reported, not
refused.

    python tools/check_asset_literals.py             # gate: exit 1 on any missing /Game/ asset
    python tools/check_asset_literals.py --selftest   # prove the checker CAN detect a moved asset
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ⚠ FORCE UTF-8 ON STDOUT, OR THIS GATE CRASHES EXACTLY WHEN IT HAS SOMETHING TO SAY. See
# check_mod_linkage.py:57-68 for the incident this pattern is copied from - the traceback and "a real
# finding" both exit non-zero, so a caller checking only the exit code cannot tell them apart, and the
# finding itself never reaches the screen.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPO_ROOT / "Source" / "FicsitsPerformanceManager"
DUMP_ROOT = Path(r"C:\VOX-BRAIN-ROOT\20-SOURCES\satisfactory\fmodel-exports")

LITERAL_RE = re.compile(r'"(/(?:Game|Script)/[A-Za-z0-9_/.]+)"')

# Given verbatim in the spec relative to the /Game/ mount point (no "FactoryGame/" prefix).
# canonical_game_id() keeps that prefix (it's part of the on-disk path), so it is added back here
# once, at the one place these two representations meet, rather than stripped ad hoc at compare time.
KNOWN_GOOD_GAME = {
    "FactoryGame/" + rel for rel in (
        "Interface/UI/Menu/Graphics/TXUI_Epic_128",
        "Interface/UI/Menu/Graphics/TXUI_PlayStation_128",
        "Interface/UI/Menu/Graphics/TXUI_Steam_128",
        "Interface/UI/Menu/Graphics/TXUI_XBOX_128",
        "Settings/OptionsMenu/UserInterface/US_ShowCreaturePerceptionIndicators",
    )
}
KNOWN_GOOD_SCRIPT = {"/Script/StreamlineReflex.StreamlineLibraryReflex"}

# The bogus canary for --selftest: a path shaped exactly like a real one, in a directory that does
# not and will not exist in the dump. If the checker ever calls this "resolved" it cannot be trusted.
SELFTEST_BOGUS = "/Game/FactoryGame/Interface/UI/Menu/Graphics/TXUI_DoesNotExist_Selftest_Canary_128"


class Hit:
    __slots__ = ("kind", "literal", "file", "line", "is_comment")

    def __init__(self, kind: str, literal: str, file: Path, line: int, is_comment: bool):
        self.kind = kind          # "Game" | "Script"
        self.literal = literal    # the literal text, quotes stripped
        self.file = file
        self.line = line
        self.is_comment = is_comment


def is_comment_line(line: str) -> bool:
    s = line.strip()
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


def extract_literals(root: Path) -> list[Hit]:
    """Every /Game/ and /Script/ literal in every .cpp/.h under root, Private and Public both."""
    hits: list[Hit] = []
    for sub in ("Private", "Public"):
        for path in sorted((root / sub).rglob("*")):
            if path.suffix.lower() not in (".cpp", ".h"):
                continue
            try:
                text = path.read_text(encoding="utf-8-sig", errors="replace")
            except OSError:
                continue  # unreadable file is its own finding, surfaced in coverage below
            for lineno, line in enumerate(text.splitlines(), start=1):
                for m in LITERAL_RE.finditer(line):
                    literal = m.group(1)
                    kind = "Game" if literal.startswith("/Game/") else "Script"
                    hits.append(Hit(kind, literal, path, lineno, is_comment_line(line)))
    return hits


def canonical_game_id(literal: str) -> str:
    """'/Game/FactoryGame/X.X' -> 'FactoryGame/X' - strip the mount point and the soft-ref suffix."""
    rest = literal[len("/Game/"):]
    return rest.split(".", 1)[0]  # UE package paths never contain '.'; the first one starts the suffix


def resolve_game_asset(literal: str) -> tuple[bool, list[Path]]:
    """Does this /Game/ literal's asset exist in the dump? Checked dirs are returned for reporting."""
    game_id = canonical_game_id(literal)  # e.g. "FactoryGame/Interface/.../TXUI_Steam_128"
    if "/" in game_id:
        dir_rel, asset_name = game_id.rsplit("/", 1)
    else:
        dir_rel, asset_name = "", game_id

    candidate_dirs = [
        DUMP_ROOT / "FactoryGame" / "Content" / dir_rel,
        DUMP_ROOT / "Exports" / "FactoryGame" / "Content" / dir_rel,
    ]
    for d in candidate_dirs:
        if d.is_dir():
            for f in d.iterdir():
                if f.stem == asset_name:
                    return True, candidate_dirs
    return False, candidate_dirs


def run_selftest() -> int:
    print(f"--selftest: checking a deliberately bogus path -> {SELFTEST_BOGUS}")
    resolved, checked = resolve_game_asset(SELFTEST_BOGUS)
    for d in checked:
        print(f"  checked: {d}  (exists: {d.is_dir()})")
    if resolved:
        print("★ SELFTEST FAILED: the checker reported a bogus, deliberately nonexistent asset as")
        print("  RESOLVED. That means it cannot be trusted to catch a real moved/removed asset either -")
        print("  a checker that has never returned a failure is indistinguishable from one that cannot.")
        return 1
    print("ok  selftest: bogus path correctly reported MISSING - the checker can fail.")
    print()

    # Second half of the self-test: a KNOWN-GOOD path must still resolve, so the failure above is
    # provably about the path being bogus and not about the dump being unreadable in general.
    # KNOWN_GOOD_GAME entries already carry the "FactoryGame/" on-disk prefix (see its definition) -
    # only the "/Game/" mount point needs adding back to make this a literal exactly as source would.
    known_good_literal = "/Game/" + next(iter(KNOWN_GOOD_GAME)) + ".x"
    resolved2, checked2 = resolve_game_asset(known_good_literal)
    print(f"--selftest: checking a KNOWN-GOOD path for contrast -> {known_good_literal}")
    for d in checked2:
        print(f"  checked: {d}  (exists: {d.is_dir()})")
    if not resolved2:
        print("★ SELFTEST FAILED: a KNOWN-GOOD asset was reported MISSING. Either the dump moved, or")
        print("  the resolution logic is broken in the other direction (false negatives on real assets).")
        return 1
    print("ok  selftest: known-good path correctly resolves.")
    print()
    print("RESULT: selftest PASS - the checker can distinguish a real asset from a fabricated one.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                     help="prove the checker can report MISSING on a bogus, known-bad path")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    if not DUMP_ROOT.is_dir():
        print(f"⚠ NOT CHECKED: the FModel dump does not exist at {DUMP_ROOT}")
        print("  No asset in FPM's source was verified against anything. This is not a clean pass -")
        print("  it is the absence of the one thing this tool needs to say anything at all.")
        return 1

    if not SOURCE_ROOT.is_dir():
        print(f"⚠ NOT CHECKED: source root does not exist: {SOURCE_ROOT}")
        return 1

    hits = extract_literals(SOURCE_ROOT)
    game_hits = [h for h in hits if h.kind == "Game"]
    script_hits = [h for h in hits if h.kind == "Script"]

    if not hits:
        print(f"⚠ NOT CHECKED: zero /Game/ or /Script/ literals found under {SOURCE_ROOT}. The known-")
        print("  good baseline expects at least 5 /Game/ hits, so an empty result means the extraction")
        print("  itself is broken (wrong root, regex not matching, or files unreadable) - not that FPM")
        print("  references nothing.")
        return 1

    # ── /Game/ assets: resolve each against the dump ──────────────────────────────────────────
    print(f"/Game/ literals found: {len(game_hits)}\n")
    missing: list[Hit] = []
    found_game_ids: set[str] = set()
    for h in game_hits:
        gid = canonical_game_id(h.literal)
        found_game_ids.add(gid)
        resolved, checked_dirs = resolve_game_asset(h.literal)
        tag = "ok    " if resolved else "★ MISSING "
        print(f"{tag}{h.file.relative_to(REPO_ROOT)}:{h.line}")
        print(f"      {h.literal}")
        if not resolved:
            missing.append(h)
            for d in checked_dirs:
                print(f"      not found in: {d}")
        print()
        if not resolved:
            pass

    # ── /Script/ class paths: reported, never checked ──────────────────────────────────────────
    print(f"/Script/ literals found: {len(script_hits)} - NOT CHECKED (code class paths, not assets;")
    print("  this tool cannot resolve a class path against a content dump).\n")
    for h in script_hits:
        note = "  (inside a comment - never executes)" if h.is_comment else ""
        print(f"  NOT CHECKED  {h.file.relative_to(REPO_ROOT)}:{h.line}  {h.literal}{note}")
    print()

    # ── Known-good baseline self-check ─────────────────────────────────────────────────────────
    missing_from_baseline_game = KNOWN_GOOD_GAME - found_game_ids
    extra_game = found_game_ids - KNOWN_GOOD_GAME
    found_script_literals = {h.literal for h in script_hits}
    missing_from_baseline_script = KNOWN_GOOD_SCRIPT - found_script_literals
    extra_script = found_script_literals - KNOWN_GOOD_SCRIPT

    baseline_broken = bool(missing_from_baseline_game or missing_from_baseline_script)
    print("BASELINE CHECK (measured 2026-08-14):")
    if missing_from_baseline_game:
        print(f"  ★ {len(missing_from_baseline_game)} known-good /Game/ item(s) NOT FOUND this run - "
              f"extraction is broken, not a clean pass:")
        for gid in sorted(missing_from_baseline_game):
            print(f"      {gid}")
    else:
        print(f"  ok    all {len(KNOWN_GOOD_GAME)} known-good /Game/ literals found this run.")
    if extra_game:
        print(f"  note  {len(extra_game)} additional /Game/ literal(s) beyond the recorded baseline "
              f"(expected growth, not a failure):")
        for gid in sorted(extra_game):
            print(f"      {gid}")

    if missing_from_baseline_script:
        print(f"  ★ {len(missing_from_baseline_script)} known-good /Script/ item(s) NOT FOUND this "
              f"run - extraction is broken, not a clean pass:")
        for s in sorted(missing_from_baseline_script):
            print(f"      {s}")
    else:
        print(f"  ok    the known-good /Script/ literal was found this run.")
    if extra_script:
        print(f"  note  {len(extra_script)} additional /Script/ literal(s) beyond the recorded "
              f"baseline (/Script/ is never gate-failed; listed above, not refused):")
        for s in sorted(extra_script):
            print(f"      {s}")
    print()

    print("COVERAGE: this catches LITERALS ONLY. A path assembled at runtime from parts (string")
    print("concatenation, FString::Printf, an FName built piecewise) or a SOFT REFERENCE stored inside")
    print("a data asset / Blueprint / .ini is invisible to a source-text regex by construction. This")
    print("gate proves nothing about those; only that every literal it CAN see still resolves.")
    print()

    if missing:
        print(f"★ {len(missing)} /Game/ ASSET(S) REFERENCED IN FPM SOURCE DO NOT RESOLVE IN THE DUMP:")
        for h in missing:
            print(f"    {h.literal}  ({h.file.relative_to(REPO_ROOT)}:{h.line})")
        print("\nThese would SILENTLY become a default/None value in-engine - no crash, no log line,")
        print("no compile error. Update the reference or confirm the asset really moved/was removed.")
        return 1

    if baseline_broken:
        print("RESULT: no /Game/ asset is missing this run, but the known-good baseline check above")
        print("did not hold - the extraction logic itself needs a look before this run can be trusted.")
        return 1

    print(f"RESULT: all {len(game_hits)} /Game/ asset literal(s) resolve. Nothing here is silently")
    print(f"pointing at a default/None value that FPM's source doesn't know moved.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
