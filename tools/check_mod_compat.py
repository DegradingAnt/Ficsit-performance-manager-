#!/usr/bin/env python3
# WHY NOT RUST: this is the Satisfactory project, whose tools/ is Python by Ant's own ruling on
# 2026-08-12 ("yeah just use python here"), and this file sits directly beside
# pull_ficsit_guides.py which already carries that same reason for the same endpoint. The job is
# one HTTP POST against the SMR GraphQL API plus a table. LAW 4: apply THIS project's rules, and
# the Rust-everywhere law is vox-scoped by its own wording.
"""Ask ficsit.app which of the INSTALLED mods the community reports as broken.

WHY THIS EXISTS: Ant spotted three broken mods by eye and said so out loud - *"these are what i
can see myself"* - meaning the list is incomplete by her own account. Eyeballing 151 mod pages is
not a method. Every mod on SMR carries a per-game-version compatibility state WITH A NOTE, and
that note is literally the "Broken by Satisfactory v1.2.4.0" text in her screenshots. Query it.

⚠ THIS IS ONE OF TWO INSTRUMENTS AND IT IS NOT SUFFICIENT ALONE. Run it beside
check_mod_linkage.py, which answers a DIFFERENT question:

    check_mod_linkage.py    PE import resolution -> "will the game START?"
                            Blind to content-only mods with no DLL, by construction.
    this tool               COMMUNITY reports -> "does it break in PLAY?"
                            Catches asset/Blueprint breakage that loads fine and then crashes.
                            Blind to anything nobody has reported yet.

A mod can pass linkage and still be Damaged - Heavy Fluid Overhaul is exactly that case. Neither
instrument sees what the other sees, so a clean result from one is not a clean bill of health.

COVERAGE IS PRINTED, ALWAYS. Silence about what a tool cannot see reads as "nothing is wrong",
which is how a dead instrument certifies the thing it was built to catch. Every run reports how
many installed mods were resolved on SMR, how many were not, and why.

    python tools/check_mod_compat.py --probe    # introspect the schema, ask nothing else
    python tools/check_mod_compat.py            # the full pass
    python tools/check_mod_compat.py --json X   # also dump raw responses to X
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ENDPOINT = "https://api.ficsit.app/v2/query"
MODS_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Satisfactory\FactoryGame\Mods")

# Mods shipped by the loader itself or authored here - they have no SMR page and their absence is
# expected, not a lookup failure. Anything else that fails to resolve is reported as UNRESOLVED.
NOT_ON_SMR = {"SML", "FicsitsPerformanceManager", "FactoryGame", "Alpakit"}


def post(query: str, variables: dict | None = None, tries: int = 3) -> dict:
    payload: dict = {"query": query}
    if variables is not None:
        payload["variables"] = variables
    body = json.dumps(payload).encode()
    last = None
    for attempt in range(tries):
        req = urllib.request.Request(
            ENDPOINT,
            data=body,
            headers={"Content-Type": "application/json", "User-Agent": "fpm-tools/1.0"},
        )
        try:
            with urllib.request.urlopen(req, timeout=45) as r:
                return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            # ⚠ THE BODY OF AN ERROR RESPONSE IS THE DIAGNOSIS, AND urllib DISCARDS IT UNLESS ASKED.
            # A 422 from this endpoint names the offending argument. An earlier version of this
            # function reported only "HTTP Error 422: Unprocessable Entity" and retried it three
            # times, which turned a one-line fix into a guess.
            try:
                detail = e.read().decode(errors="replace")[:600]
            except Exception:
                detail = "(body unreadable)"
            last = "HTTP %s: %s" % (e.code, detail)
            # 4xx is a defect in the request. Retrying it just costs time and hides the cause.
            if 400 <= e.code < 500:
                raise RuntimeError("SMR rejected the request -- %s" % last) from None
            time.sleep(1.5 * (attempt + 1))
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError("SMR request failed after %d tries: %s" % (tries, last))


def probe() -> None:
    """Print the schema shape instead of guessing field names.

    A guessed field name that comes back as an error looks exactly like a field that does not
    exist. Asking the schema is the difference between evidence and a hypothesis.
    """
    q = """
    query($n:String!){ __type(name:$n){ name kind
      enumValues { name }
      fields { name type { name kind ofType { name kind ofType { name kind } } } } } }
    """
    for name in ("Mod", "Compatibility", "CompatibilityInfo", "Query"):
        d = post(q, {"n": name})
        t = (d.get("data") or {}).get("__type")
        if not t:
            print("== %s :: NOT PRESENT  %s" % (name, json.dumps(d)[:200]))
            print()
            continue
        print("== %s (%s)" % (t["name"], t["kind"]))
        for ev in t.get("enumValues") or []:
            print("   enum: %s" % ev["name"])
        for f in t.get("fields") or []:
            ty = f["type"]
            nm = ty.get("name")
            while not nm and ty.get("ofType"):
                ty = ty["ofType"]
                nm = ty.get("name")
            flag = "  <<<" if "compat" in f["name"].lower() else ""
            print("   %-36s %s%s" % (f["name"], nm, flag))
        print()


def installed_mods() -> list[str]:
    """Enumerate by *.uplugin RECURSIVELY.

    A top-level directory listing under-reports badly - mods nest under GameFeatures/ subfolders.
    Measured 2026-08-15: 62 top-level dirs against 151 real mods.
    """
    if not MODS_DIR.is_dir():
        raise SystemExit("mods dir not found: %s" % MODS_DIR)
    return sorted({p.stem for p in MODS_DIR.rglob("*.uplugin")})


# Field set verified against the live schema by --probe on 2026-08-15, not guessed:
#   Mod.compatibility -> CompatibilityInfo { EA: Compatibility, EXP: Compatibility }
#   Compatibility     -> { state: CompatibilityState, note: String }
# ⚠ EA AND EXP ARE SEPARATE STATES. EA is the stable branch, EXP experimental. A mod can be Broken
# on one and Works on the other, so asking for a single "is it broken" answer is branch-blind and
# would silently report the wrong branch's verdict.
# source_url comes back in the same round trip and is what sf-upstream-source needs for the
# registry, so the repo lookup costs nothing extra here.
MOD_QUERY = """
query($ref:ModReference!){
  getModByReference(modReference:$ref){
    id name mod_reference source_url hidden
    compatibility { EA { state note } EXP { state note } }
  }
}
"""


def fetch_one(ref: str) -> dict | None:
    d = post(MOD_QUERY, {"ref": ref})
    if d.get("errors"):
        return {"_error": json.dumps(d["errors"])[:200]}
    return (d.get("data") or {}).get("getModByReference")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", action="store_true", help="introspect the schema and stop")
    ap.add_argument("--one", metavar="REF", help="query a single mod reference and dump it raw")
    ap.add_argument("--json", metavar="PATH", help="dump raw responses here")
    a = ap.parse_args()

    if a.probe:
        probe()
        return 0

    if a.one:
        print(json.dumps(fetch_one(a.one), indent=2))
        return 0

    mods = installed_mods()
    print("installed mods (recursive *.uplugin): %d" % len(mods))

    rows: list[tuple] = []
    unresolved: list[str] = []
    skipped: list[str] = []
    raw: dict = {}

    for i, ref in enumerate(mods, 1):
        if ref in NOT_ON_SMR:
            skipped.append(ref)
            continue
        try:
            m = fetch_one(ref)
        except Exception as e:
            unresolved.append("%s (request failed: %s)" % (ref, e))
            continue
        if not m or m.get("_error"):
            unresolved.append("%s%s" % (ref, " -- " + m["_error"] if m and m.get("_error") else ""))
            continue
        raw[ref] = m
        comp = m.get("compatibility") or {}
        ea = (comp.get("EA") or {}) if comp else {}
        exp = (comp.get("EXP") or {}) if comp else {}
        rows.append(
            (ref, m.get("name") or "", ea.get("state"), ea.get("note") or "",
             exp.get("state"), exp.get("note") or "", m.get("source_url") or "")
        )
        if i % 25 == 0:
            print("  ... %d/%d" % (i, len(mods)), file=sys.stderr)

    if a.json:
        Path(a.json).write_text(json.dumps(raw, indent=2), encoding="utf-8")

    # ---- the finding: anything not Works, on either branch -------------------------------------
    bad = [r for r in rows if (r[2] and r[2] != "Works") or (r[4] and r[4] != "Works")]
    bad.sort(key=lambda r: (r[2] != "Broken", r[4] != "Broken", r[0].lower()))

    print()
    print("=" * 100)
    print("NOT 'Works' ON AT LEAST ONE BRANCH: %d" % len(bad))
    print("=" * 100)
    for ref, name, eas, ean, exs, exn, src in bad:
        print("\n%-40s %s" % (ref, name))
        print("   EA  %-8s %s" % (eas or "-", ean))
        print("   EXP %-8s %s" % (exs or "-", exn))
        print("   src %s" % (src or "(none declared)"))

    # ---- coverage, printed every run, pass or fail ---------------------------------------------
    # An instrument that stays silent about what it could not see reads as a clean bill of health.
    stated = sum(1 for r in rows if r[2] or r[4])
    print()
    print("=" * 100)
    print("COVERAGE  (this instrument sees COMMUNITY REPORTS only)")
    print("=" * 100)
    print("  installed and enumerated      %4d" % len(mods))
    print("  skipped, no SMR page by design%4d   %s" % (len(skipped), ", ".join(skipped)))
    print("  resolved on SMR               %4d" % len(rows))
    print("  ... of those, WITH a state    %4d" % stated)
    print("  ... of those, state UNSET     %4d   <- SMR knows the mod, nobody has reported a state"
          % (len(rows) - stated))
    print("  UNRESOLVED                    %4d" % len(unresolved))
    for u in unresolved:
        print("      %s" % u)
    print()
    print("  ⚠ BLIND SPOTS, BY CONSTRUCTION:")
    print("     - a state is a COMMUNITY REPORT. An unreported break reads identically to healthy.")
    print("     - a mod with no state is NOT a pass. It is NOT CHECKED.")
    print("     - asset/Blueprint breakage that loads fine is invisible to check_mod_linkage.py;")
    print("       DLL import breakage is invisible here. RUN BOTH.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
