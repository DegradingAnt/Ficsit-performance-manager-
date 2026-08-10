# WHY NOT RUST: this ships INSIDE the public GPL-3.0 mod repo, not in the brain's 40-TOOLS. A
# contributor cloning it has UE 5.6 and VS 2022, not a cargo toolchain; adding a Rust workspace to a
# game-mod repo so it can lint its own metadata is a heavier dependency than the lint, and committing a
# prebuilt binary would contradict this repo's own "ships no binaries" rule. Stdlib Python 3 runs
# everywhere the modding toolchain already does.
#!/usr/bin/env python3
"""Structure and metadata gate for FICSIT's Performance Manager.

WHY THIS EXISTS. The rewrite from FPM1 to FPM2 silently dropped things, repeatedly, and each one was
found by accident weeks later:

  * three working fixes (the Wwise server audio gate among them, found only because its warnings turned
    up while somebody was reading an unrelated crash log),
  * README.md, LICENSE and CONTRIBUTING.md, all three, found only when somebody asked whether the repo
    followed community structure,
  * a stale `GameVersion` that a written audit had already flagged in July and that survived the whole
    rewrite unfixed.

Every one of those was covered by a checklist that existed. A checklist nobody runs is how the same
mistake happens twice, so this is the checklist as code. Run it before opening a PR and before
packaging:

    python tools/check_structure.py            # exit 1 on any ERROR
    python tools/check_structure.py --warnings-as-errors

It reads the repo it lives in, so it cannot drift onto a stale copy of the tree.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MODULE = "FicsitsPerformanceManager"
SRC = REPO / "Source" / MODULE

# The game build this mod is actually tested against. Bump WITH the boot test, never ahead of it.
TESTED_GAME_CL = 495413

ERRORS: list[str] = []
WARNINGS: list[str] = []


def err(msg: str) -> None:
    ERRORS.append(msg)


def warn(msg: str) -> None:
    WARNINGS.append(msg)


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8-sig", errors="replace")


# --------------------------------------------------------------------------------------------------
# 1. The files the community expects a mod repo to carry.
# --------------------------------------------------------------------------------------------------
def check_required_files() -> None:
    required = {
        "README.md": "what the mod does, requirements, hard rules, transparency",
        "LICENSE": "the licence text itself — every source header claims GPL-3.0, so it must ship",
        "CHANGELOG.md": "append-only, newest first, one entry per discrete change",
        "CONTRIBUTING.md": "setup, the verify-before-you-claim discipline, the paperwork rules",
        f"{MODULE}.uplugin": "the descriptor SMM and ficsit.app act on",
        "Resources/Icon128.png": "required by the plugin format and shown on the mod page",
    }
    for rel, why in required.items():
        if not (REPO / rel).exists():
            err(f"MISSING {rel} — {why}")

    lic = REPO / "LICENSE"
    if lic.exists() and "GNU GENERAL PUBLIC LICENSE" not in read(lic):
        err("LICENSE is present but is not the GNU GPL text, while the source headers claim GPL-3.0")


# --------------------------------------------------------------------------------------------------
# 2. The descriptor. Each of these was a real finding at least once.
# --------------------------------------------------------------------------------------------------
def check_uplugin() -> dict:
    path = REPO / f"{MODULE}.uplugin"
    if not path.exists():
        return {}
    try:
        d = json.loads(read(path))
    except json.JSONDecodeError as e:
        err(f"{MODULE}.uplugin is not valid JSON: {e}")
        return {}

    version_name, sem = d.get("VersionName"), d.get("SemVersion")
    if version_name != sem:
        err(f"VersionName ({version_name!r}) != SemVersion ({sem!r}) — they must always be equal")

    if isinstance(sem, str) and re.fullmatch(r"\d+\.\d+\.\d+", sem):
        want = int(sem.split(".")[0])
        if d.get("Version") != want:
            err(f"Version is {d.get('Version')!r} but SemVersion {sem} says it should be {want}")
    else:
        err(f"SemVersion {sem!r} is not a MAJOR.MINOR.PATCH string")

    gv = str(d.get("GameVersion", ""))
    m = re.search(r"(\d{5,})", gv)
    if not m:
        err(f"GameVersion {gv!r} names no changelist")
    elif int(m.group(1)) < TESTED_GAME_CL:
        err(
            f"GameVersion {gv!r} trails the tested build CL {TESTED_GAME_CL}. "
            "This exact defect was written up on 2026-07-17 and survived the whole rewrite."
        )

    # SML enforces exact-equality parity for a RequiredOnRemote mod, so the pin has to track the version.
    if d.get("RequiredOnRemote"):
        rvr = d.get("RemoteVersionRange")
        if rvr != f"={sem}":
            err(
                f"RequiredOnRemote is true but RemoteVersionRange is {rvr!r}, not '={sem}'. "
                "A stale pin refuses every join or, worse, admits a mismatched pair."
            )

    mods = d.get("Modules") or []
    if len(mods) != 1 or mods[0].get("Name") != MODULE:
        err(f"expected exactly one module named {MODULE}, found {[m.get('Name') for m in mods]}")
    elif mods[0].get("Type") != "Runtime" or mods[0].get("LoadingPhase") != "Default":
        err("the runtime module must be Type=Runtime, LoadingPhase=Default")

    # Alpakit strips SemVersion from anything marked BasePlugin (ModMetadataObject.cpp:170-180), so a
    # game-shipped plugin carrying one is a declaration that will never be honoured, and a ficsit.app
    # dependency WITHOUT one is unresolvable.
    for p in d.get("Plugins") or []:
        name, base, has_sem = p.get("Name"), p.get("BasePlugin"), "SemVersion" in p
        if base and has_sem:
            err(f"plugin {name!r} is BasePlugin but carries SemVersion — Alpakit will strip it")
        if not base and not has_sem:
            err(f"plugin {name!r} has neither BasePlugin nor SemVersion; it cannot be resolved")

    for field in ("DocsURL", "SupportURL", "CreatedByURL"):
        if not d.get(field):
            warn(f"{field} is empty — ficsit.app expects a support/contact route on the mod page")

    return d


# --------------------------------------------------------------------------------------------------
# 3. The fix contract, and the orphan check that is the whole point of this file.
# --------------------------------------------------------------------------------------------------
CONTRACT = ("Name", "Side", "OriginStatus", "Channel")


def collect_fixes() -> dict[str, Path]:
    """Every header implementing IFPMFix -> its declared fix name."""
    found: dict[str, Path] = {}
    for h in sorted(SRC.glob("Public/**/*.h")):
        s = read(h)
        if "public IFPMFix" not in s:
            continue
        for member in CONTRACT:
            if not re.search(rf"\b{member}\(\)\s*const\s+override", s):
                err(f"{h.relative_to(REPO)} implements IFPMFix but never overrides {member}()")
        m = re.search(r'Name\(\)\s*const\s+override\s*\{\s*return\s+TEXT\("([^"]+)"\)', s)
        if m:
            found[m.group(1)] = h
        else:
            warn(f"{h.relative_to(REPO)} implements IFPMFix but its Name() is not a plain literal")
    return found


def check_every_fix_is_armed(fixes: dict[str, Path]) -> None:
    """A fix that exists but is never armed is dead code that reads as a shipped feature."""
    startup = SRC / "Private" / f"{MODULE}.cpp"
    if not startup.exists():
        err(f"cannot find {startup.relative_to(REPO)}")
        return
    s = read(startup)
    armed = set(re.findall(r"FPMFixes::Arm\(\s*(\w+)::Get\(\)\s*\)", s))
    for name, header in fixes.items():
        cls = re.search(r"class\s+(\w+)\s+final\s*:\s*public\s+IFPMFix", read(header))
        if cls and cls.group(1) not in armed:
            err(
                f"fix {name!r} ({cls.group(1)}) is never passed to FPMFixes::Arm — it compiles, "
                "ships, and does nothing"
            )


# The fixes FPM1 shipped. The rewrite was supposed to carry every one of them; three were dropped
# silently and one of those sat missing for two days on a live server. Anything not present in FPM2
# must be here with a stated reason, so "we forgot" and "we decided" stop looking identical.
FPM1_DISPOSITION = {
    "RegisterCloneJoinDiagnostics": "carried as clone-sensor",
    "RegisterContactShadowSuppressor": "DROPPED by ruling — Ant 2026-08-09, not needed",
    "RegisterCrashingHookBlocker": "carried as hud-hook-guard, rebuilt narrow",
    "RegisterHologramNetGuard": "carried as hologram-net",
    "RegisterInventoryInitGuard": "carried as inventory-init",
    "RegisterNavMeshCoverageFix": "carried as navmesh-ceiling",
    "RegisterRailConnectionGuard": "carried as rail-connection-guard",
    "RegisterRainOcclusionFix": "carried as rain-occlusion",
    "RegisterSchematicCrashBreadcrumb": "carried as schematic-probe + schematic-null-guard",
    "RegisterStaticBaseFix": "carried as static-base",
    "RegisterStatsSignRpcGate": "carried as no-owner-rpc-gate, widened past the Stats-sign class",
    "RegisterTrainLodPin": "DROPPED by ruling — Ant 2026-08-09, the old LOD transition was itself the bug",
    "RegisterWwiseServerAudioGate": "carried as wwise-server-gate (was ORPHANED for 2 days — the reason this check exists)",
    "RegisterZiplineVolumeFix": "carried as zipline-volume",
}


def check_predecessor_coverage() -> None:
    fpm1 = (
        REPO.parent.parent
        / "FicsitPerformanceManager"
        / "Source"
        / "FicsitPerformanceManager"
        / "Private"
        / "FicsitPerformanceManager.cpp"
    )
    if not fpm1.exists():
        warn(f"FPM1 source not found at {fpm1} — cannot re-check predecessor coverage")
        return
    # NB: matches Register<Anything>(), not Register<Anything>Fix(). Narrowing this pattern to
    # 'Fix()' is the exact grep that once reported "FPM1 has only 4 fixes" and hid the orphaned gate.
    live = set(re.findall(r"\bRegister([A-Za-z]+)\(\)", read(fpm1)))
    for name in sorted(live):
        if f"Register{name}" not in FPM1_DISPOSITION:
            err(
                f"FPM1 registers Register{name}() and FPM2's disposition table says nothing about it. "
                "Carry it, or record why not — do not leave it unanswered."
            )


# --------------------------------------------------------------------------------------------------
# 4. The diagnostics table. Its own static_assert checks COUNT only, by its own admission.
# --------------------------------------------------------------------------------------------------
def check_diag_table() -> None:
    hdr, cpp = SRC / "Public/Core/FPMDiag.h", SRC / "Private/Core/FPMDiag.cpp"
    if not (hdr.exists() and cpp.exists()):
        err("FPMDiag.h / FPMDiag.cpp not found")
        return
    block = re.search(r"enum class EChannel\s*:\s*uint8\s*\{(.*?)Count", read(hdr), re.S)
    if not block:
        err("could not parse FPMDiag::EChannel")
        return
    channels = [m.group(1) for m in re.finditer(r"^\s*(\w+),", block.group(1), re.M)]

    body = read(cpp)
    table = re.search(r"GChannelCVars\[\]\s*=\s*\{(.*?)\};", body, re.S)
    cvars = re.findall(r"&CVar\w+", table.group(1)) if table else []
    cases = re.findall(r"case FPMDiag::EChannel::(\w+):", body)

    if len(channels) != len(cvars):
        err(f"EChannel has {len(channels)} entries but GChannelCVars has {len(cvars)}")
    missing = [c for c in channels if c not in cases]
    if missing:
        err(f"ChannelName() has no case for: {', '.join(missing)} — they will print as <unknown>")


# --------------------------------------------------------------------------------------------------
# 5. Hard rules that are checkable from the source.
# --------------------------------------------------------------------------------------------------
NETWORK_PATTERNS = [
    (r"\bFHttpModule\b", "HTTP"),
    (r"\bIHttpRequest\b", "HTTP"),
    (r"\bFSocket\b", "sockets"),
    (r"\bISocketSubsystem\b", "sockets"),
    (r"\bFAnalytics\b", "analytics"),
    (r"\bFTelemetry", "telemetry"),
]


def check_no_network() -> None:
    """Hard rule 2. Stated in the README, promised on the mod page, and never checked until now."""
    for f in sorted(SRC.rglob("*.cpp")) + sorted(SRC.rglob("*.h")):
        s = read(f)
        for pat, what in NETWORK_PATTERNS:
            if re.search(pat, s):
                err(f"{f.relative_to(REPO)} references {what} — hard rule 2 says no network activity, ever")


def check_gamefeature_data() -> None:
    """The GameFeature asset must know about every Content/ root, or the assets there never register.

    `Content/<Plugin>.uasset` is an `FGGameFeatureData`. FactoryGame requires it for any plugin under
    `Mods/GameFeatures/`, and it names the content roots that get scanned. Ant, 2026-08-10: *"it should
    be updated along with the rest of the mod then."*

    The failure this catches is silent and expensive: add `Content/Settings/` for the P4 settings
    surface, forget the asset, and the rows simply do not appear in game with no error anywhere. The
    asset is binary, so this reads it as bytes and looks for each directory name — crude, but it is the
    difference between a loud check and no check.
    """
    asset = REPO / "Content" / f"{MODULE}.uasset"
    if not asset.exists():
        err(
            f"Content/{MODULE}.uasset is missing. A plugin under Mods/GameFeatures/ needs its "
            "FGGameFeatureData asset or the GameFeature never activates."
        )
        return

    blob = asset.read_bytes()
    if b"FGGameFeatureData" not in blob:
        err(f"Content/{MODULE}.uasset does not look like an FGGameFeatureData asset")

    for sub in sorted(p for p in (REPO / "Content").iterdir() if p.is_dir()):
        if sub.name.encode() not in blob:
            err(
                f"Content/{sub.name}/ exists but {MODULE}.uasset never names it. Assets under it will "
                "not be registered, and nothing will say so at runtime."
            )


def check_licence_headers() -> None:
    for f in sorted(SRC.rglob("*.cpp")) + sorted(SRC.rglob("*.h")):
        if f.name.endswith(".g.h"):
            continue
        lines = read(f).lstrip().splitlines()[:1]
        if not lines or "GPL-3.0" not in lines[0]:
            err(f"{f.relative_to(REPO)} is missing the GPL-3.0 copyright header")


def check_raw_subscribe() -> None:
    """Hard rule 6: a hook that skips the ledger is a hook the inventory lies about."""
    for f in sorted(SRC.rglob("*.cpp")):
        for i, line in enumerate(read(f).splitlines(), 1):
            stripped = line.lstrip()
            if stripped.startswith(("*", "//", "/*")):
                continue
            if re.search(r"(?<!FPM_)\bSUBSCRIBE_METHOD\w*\s*\(", line) and "FPMHookLedger" not in line:
                err(f"{f.relative_to(REPO)}:{i} uses a raw SUBSCRIBE_ macro — go through FPM_SUBSCRIBE*")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--warnings-as-errors", action="store_true")
    args = ap.parse_args()

    check_required_files()
    check_uplugin()
    fixes = collect_fixes()
    check_every_fix_is_armed(fixes)
    check_predecessor_coverage()
    check_diag_table()
    check_gamefeature_data()
    check_no_network()
    check_licence_headers()
    check_raw_subscribe()

    for w in WARNINGS:
        print(f"WARN  {w}")
    for e in ERRORS:
        print(f"ERROR {e}")

    print(f"\n{len(fixes)} fixes · {len(ERRORS)} error(s) · {len(WARNINGS)} warning(s)")
    if ERRORS or (args.warnings_as_errors and WARNINGS):
        return 1
    print("structure OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
