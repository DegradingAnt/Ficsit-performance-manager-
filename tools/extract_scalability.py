#!/usr/bin/env python3
# WHY NOT RUST: same reason as package_fpm.py and check_mod_linkage.py beside it — this is the
# Satisfactory project, whose tools/ is Python, and the job is ini parsing plus a TSV. LAW 4: apply
# THIS project's rules, and the Rust-everywhere law is vox-scoped by its own wording. Ant confirmed it
# directly on 2026-08-12: "yeah just use python here". It was offered a home in the brain's Rust
# workspace (40-TOOLS/rust) and declined one — that workspace is brain INFRASTRUCTURE (hooks, index,
# lint), and an FPM table extractor is not brain infrastructure.
"""Extract the scalability group expansions (sg.* -> member cvars) THIS GAME actually applies.

═══ WHY THIS EXISTS, AND WHY IT NEARLY DID NOT ═══

FPM's drift watch (Private/Core/FPMSupport.cpp) names three tables that go stale when the game's
changelist moves. This is the third of them: the sg.* group expansions. Until 2026-08-12 it stood
recorded as NOT DERIVABLE AT A DESK. From FPMCVarProbe.cpp:26-34, written after an afternoon lost to
exactly that:

    every candidate list came from the ENGINE's Engine/Config/BaseScalability.ini, which is NOT the
    table this game applies. Proven from her running game: at sg.ShadowQuality 2 the console's
    HISTORY readout showed  Constructor: 2048  Scalability: 512  Console: 1024  for
    r.Shadow.MaxResolution, while the engine ini says [ShadowQuality@2] ...=1024. Satisfactory ships
    its own scalability table, there is no loose copy of it anywhere in the install, and so the only
    place the truth exists is INSIDE THE RUNNING GAME.

That cost seven wrong hypotheses about a ~65 fps drop in Ant's base on 2026-08-09.

★ THAT STATEMENT WAS TRUE OF THE INSTALL AND IS NO LONGER TRUE OF WHAT WE HAVE. Satisfactory's own
`FactoryGame/Config/DefaultScalability.ini` is COOKED INTO THE PAK, so no amount of looking through
the install directory ever finds it. FModel extracts it, and the dump Ant took on 2026-08-11 has it.

⚠ THE OLD NOTE IS NOT WRONG, IT IS SCOPED. The truth was unreachable with the instruments of
2026-08-09. Read this as that finding being overtaken by a new instrument, not corrected. Epic's
BaseScalability.ini is still the wrong answer and always was — it is used here only as the FALLBACK
for groups Satisfactory does not override at all.

The layering is Epic's documented model, not a guess: "BaseScalability.ini can be overwritten by
DefaultScalability.ini", sections keyed `[<group>@<level>]`.
    https://dev.epicgames.com/documentation/unreal-engine/scalability-reference-for-unreal-engine

═══ THE TEST THAT MAKES THIS TRUSTWORTHY ═══

A file that merely LOOKS like the right table is not evidence — Epic's copy looks just as convincing,
and looking convincing is precisely how it wasted an afternoon. So this checks itself against the one
datum measured from the running game:

    [ShadowQuality@2]  r.Shadow.MaxResolution  must read 512, NOT Epic's 1024

If it does not, the script REFUSES to emit rather than shipping a plausible wrong answer. That is a
known-positive test: it can tell "found the real table" apart from "found another copy of Epic's",
which a mere file-exists check cannot.

⚠ ONE DATUM IS ONE DATUM, AND IT IS FROM THE OLD BUILD. It was measured on CL 495413; this dump is
CL 502094. A legitimate CSS change to that exact value would read here as a failure. The failure
message says so, so nobody concludes "wrong file" when the right answer is "the value moved".

    python tools/extract_scalability.py            # write the TSV, report what changed
    python tools/extract_scalability.py --check    # verify only, write nothing
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

DUMP = Path(r"C:\VOX-BRAIN-ROOT\20-SOURCES\satisfactory\fmodel-exports")
BASE_INI = DUMP / "Engine" / "Config" / "BaseScalability.ini"          # Epic's — FALLBACK ONLY
GAME_INI = DUMP / "FactoryGame" / "Config" / "DefaultScalability.ini"  # Satisfactory's — AUTHORITY
OUT_TSV = Path(__file__).with_name("sg_expansions.tsv")

# The known positive, measured from Ant's running game 2026-08-09 (FPMCVarProbe.cpp:30-32).
CANARY_SECTION = "ShadowQuality@2"
CANARY_CVAR = "r.Shadow.MaxResolution"
CANARY_GAME = "512"     # what the game's Scalability layer actually applied
CANARY_EPIC = "1024"    # Epic's value — the one that misled seven hypotheses

SECTION_RE = re.compile(r"^\[([A-Za-z]+@[0-9A-Za-z]+)\]\s*$")


def parse(path: Path) -> dict[str, dict[str, str]]:
    """Section -> {cvar: value}. Later keys win within a section, as UE's ini layering does."""
    out: dict[str, dict[str, str]] = {}
    cur: dict[str, str] | None = None
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        m = SECTION_RE.match(line)
        if m:
            cur = out.setdefault(m.group(1), {})
            continue
        if cur is not None and "=" in line:
            k, _, v = line.partition("=")
            cur[k.strip()] = v.strip()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify the canary and report; write nothing")
    args = ap.parse_args()

    for p in (BASE_INI, GAME_INI):
        if not p.exists():
            print(f"FAIL  missing {p}")
            print("      Re-dump the game with FModel, or fix the path. NOT emitting a partial table —")
            print("      a half-derived expansion table is worse than none, because it reads complete.")
            return 1

    base, game = parse(BASE_INI), parse(GAME_INI)

    # ── THE KNOWN-POSITIVE TEST. Refuse rather than emit a plausible wrong table. ────────────────
    got = game.get(CANARY_SECTION, {}).get(CANARY_CVAR)
    if got != CANARY_GAME:
        print(f"FAIL  canary: [{CANARY_SECTION}] {CANARY_CVAR} = {got!r}, expected {CANARY_GAME!r}")
        if got == CANARY_EPIC:
            print(f"      That is EPIC's value. {GAME_INI.name} is not this game's table, or the dump")
            print("      picked up the engine copy under a game path. REFUSING to emit — this is the")
            print("      exact wrong answer that cost seven hypotheses on 2026-08-09.")
        else:
            print("      Neither the game's known value nor Epic's. The value may legitimately have")
            print("      MOVED between CL 495413, when it was measured, and this dump. Check that against")
            print("      a running game before concluding this file is the wrong one.")
        return 1
    print(f"ok    canary: [{CANARY_SECTION}] {CANARY_CVAR} = {got}  "
          f"(Epic's base says {CANARY_EPIC} — so this IS the game's table, not the engine's)")

    # ── Merge: Epic's base is the floor, the game's file overrides per key, as UE layers them. ───
    merged: dict[str, dict[str, str]] = {s: dict(v) for s, v in base.items()}
    overridden = 0
    for sect, cvars in game.items():
        tgt = merged.setdefault(sect, {})
        for k, v in cvars.items():
            if k in tgt and tgt[k] != v:
                overridden += 1
            tgt[k] = v

    groups = sorted({s.split("@")[0] for s in merged})
    game_only = sorted({s.split("@")[0] for s in game} - {s.split("@")[0] for s in base})
    rows = sum(len(v) for v in merged.values())

    print(f"ok    {len(groups)} group(s), {len(merged)} group@level section(s), {rows} cvar assignment(s)")
    print(f"ok    {overridden} key(s) where Satisfactory overrides Epic's value")
    print(f"ok    {len(game_only)} group(s) that exist ONLY in Satisfactory: {', '.join(game_only) or '(none)'}")

    if args.check:
        print("\n--check: nothing written.")
        return 0

    prev_rows = (len(OUT_TSV.read_text(encoding="utf-8").splitlines()) - 1) if OUT_TSV.exists() else None

    lines = ["group\tlevel\tcvar\tvalue\tsource"]
    for sect in sorted(merged):
        grp, _, lvl = sect.partition("@")
        for cvar in sorted(merged[sect]):
            src = "game" if cvar in game.get(sect, {}) else "engine"
            lines.append(f"{grp}\t{lvl}\t{cvar}\t{merged[sect][cvar]}\t{src}")
    OUT_TSV.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\nwrote {OUT_TSV.name}  ({len(lines) - 1} rows)")
    if prev_rows is not None:
        print(f"      previous file had {prev_rows} rows; delta {len(lines) - 1 - prev_rows:+d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
