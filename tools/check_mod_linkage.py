#!/usr/bin/env python3
# WHY NOT RUST: same reason as package_fpm.py and deploy_fpm.py beside it - this is the Satisfactory
# project, whose tools/ is Python, and pefile is already installed and does the whole job. LAW 4:
# apply THIS project's rules; the Rust-everywhere law is vox-scoped by its own wording.
"""Name every installed mod the current game build has BROKEN, by symbol, without booting anything.

═══ WHY THIS EXISTS ═══

Satisfactory updated on 2026-08-11 from CL 495413 to **CL 502094**, and the Steam client rewrote the
game's binaries at 18:30:17 - three minutes into a boot test that had launched at 18:29. The first
launch ran on the old build and loaded fine. Every launch after it ran a NEW game against mods built
for the OLD one, and died on a modal Windows dialog before the engine finished starting:

    Entry Point Not Found
    The procedure entry point ?OnDismantleEffectFinished@AFGBuildable@@UEAAXXZ could not be located
    in ...\\Mods\\EfficiencyCheckerMod\\...\\FactoryGameSteam-EfficiencyCheckerMod-Win64-Shipping.dll

⚠ THAT DIALOG IS THE ONLY DIAGNOSTIC THE GAME GIVES YOU, AND IT NAMES EXACTLY ONE MOD. The loader
stops at the first unresolved import, so a stack with four broken mods reports one, gets that one
removed, and reports the next. Four boots to learn what this script reads in seconds.

★ AND THE VERSION GATE DOES NOT CATCH THIS. SML 3.12.0 declares `"GameVersion": ">=491125"` - an open
lower bound - so 502094 satisfies it and SML loads happily. The break is one level below the version
check, at native linkage: a mod imports a mangled C++ symbol, CSS changed or removed that function,
and the Windows loader refuses the DLL. **A green version check is not evidence a mod will load.**

═══ WHAT IT DOES ═══

Reads the PE import table of every mod binary and resolves each import against the export tables of
the game's own module DLLs (this is a MODULAR build - 181 of them). Anything unresolved is a mod that
CANNOT load on this game build, reported with the symbol and a readable Class::Function rendering of
the MSVC mangling.

  python tools/check_mod_linkage.py            # gate: exit 1 if any mod is broken
  python tools/check_mod_linkage.py --verbose  # also list every mod that resolves clean

⚠ STOREFRONT SPLIT. Mods ship both `FactoryGameSteam-*` and `FactoryGameEGS-*` binaries, and only one
storefront's game DLLs exist in the install. Checking an EGS binary against a Steam install reports
every single import as missing - a 100% false-positive rate that would bury the real answer. The
storefront is DETECTED from the install and only matching binaries are checked; the others are counted
and reported as skipped, never as passing (LAW: "no reference available" is NOT CHECKED, never a pass).

⚠ MODS IMPORT FROM EACH OTHER TOO. KBFL and RefinedRDLib are libraries other mods link against, so the
export universe must include the mod binaries, not just the game's. Otherwise every consumer of a
library mod is reported broken.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import pefile

# ⚠ FORCE UTF-8 ON STDOUT, OR THIS GATE CRASHES EXACTLY WHEN IT HAS SOMETHING TO SAY.
#
# Measured 2026-08-12: run from a cp1252 console, the tool completed its whole analysis and then died
# with UnicodeEncodeError on the U+2605 in "★ n MOD(S) CANNOT LOAD" - the one line that carries the
# finding. Every diagnostic line before it printed fine, so the failure looks like a crash in the
# reporting code rather than what it is: a console that cannot represent the output.
#
# That is the worst possible failure shape for a GATE. The traceback exits non-zero, which is the same
# exit code as "broken mods found", so a caller checking only the exit code cannot tell a real finding
# from a dead tool - and the finding itself never reaches the screen.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

GAME_ROOT = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Satisfactory")
GAME_BINARIES = GAME_ROOT / "Engine" / "Binaries" / "Win64"
MODS_ROOT = GAME_ROOT / "FactoryGame" / "Mods"


def installed_mod_names() -> set[str]:
    """Every installed mod, native or not.

    Keyed on the .uplugin because that is the one file EVERY mod has - a content-only mod has no
    Binaries/ directory at all, so enumerating DLLs silently under-counts the stack by more than half.
    """
    return {up.stem for up in MODS_ROOT.rglob("*.uplugin")}

# ?FuncName@ClassName@@<encoding>  ->  ClassName::FuncName
MANGLED = re.compile(rb"^\?(\w+)@(\w+)@")


def readable(sym: bytes) -> str:
    """Render an MSVC-mangled name as Class::Function. Best effort - the raw symbol is always shown."""
    m = MANGLED.match(sym)
    return f"{m.group(2).decode()}::{m.group(1).decode()}" if m else ""


def exports_of(path: Path) -> set[bytes]:
    """Read the export NAME TABLE directly. Do NOT use pefile.DIRECTORY_ENTRY_EXPORT here.

    ⚠ THIS FUNCTION EXISTS BECAUSE THE OBVIOUS VERSION IS SILENTLY WRONG, AND IT ALMOST SHIPPED A
    CATASTROPHIC ANSWER. pefile refuses to parse more than `MAX_SYMBOL_EXPORT_COUNT` = **8192**
    export names and warns *"Export directory contains more than 8192 symbol entries. Assuming
    corrupt."* - a warning, not an exception, so a caller that only reads `.symbols` gets a TRUNCATED
    set and no error. `FactoryGameSteam-Engine-Win64-Shipping.dll` exports **32,686** names, so 75% of
    the engine looked absent and the first run of this script reported **50 of 59 mods broken**,
    including on `AActor::Tick` and `AActor::GetWorld` - functions whose absence would mean the engine
    itself could not run.

    Raising `pefile.MAX_SYMBOL_EXPORT_COUNT` does NOT help; it is captured internally and the count
    came back as exactly 8192 again. So the name table is read directly, which is cap-free and gets
    all 32,686. pefile is still used, but only for RVA-to-offset mapping.

    A parse failure returns None - NEVER an empty set. An empty set would be read as "this module
    exports nothing", which is an absence claim, and absence claims from a broken reader are the exact
    fault this docstring is about.
    """
    try:
        pe = pefile.PE(str(path), fast_load=True)
        dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]
        ]
        if not dd.VirtualAddress or not dd.Size:
            pe.close()
            return set()  # genuinely exports nothing - a real, readable answer
        count = pe.get_dword_at_rva(dd.VirtualAddress + 0x18)   # NumberOfNames
        table = pe.get_dword_at_rva(dd.VirtualAddress + 0x20)   # AddressOfNames -> RVA[]
        out = set()
        for i in range(count):
            name = pe.get_string_at_rva(pe.get_dword_at_rva(table + 4 * i))
            if name:
                out.add(name)
        pe.close()
        return out
    except Exception as exc:
        print(f"  !! could not read exports from {path.name}: {exc}")
        return None


def imports_of(path: Path) -> dict[str, list[bytes]]:
    """{imported dll name (lowercased): [symbol, ...]}. Ordinal-only imports are ignored."""
    result: dict[str, list[bytes]] = {}
    try:
        pe = pefile.PE(str(path), fast_load=True)
        pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]]
        )
        for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            names = [i.name for i in entry.imports if i.name]
            if names:
                result.setdefault(entry.dll.decode().lower(), []).extend(names)
        pe.close()
    except Exception as exc:
        print(f"  !! could not read imports from {path.name}: {exc}")
    return result


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--verbose", action="store_true", help="also list mods that resolve clean")
    args = ap.parse_args()

    if not GAME_BINARIES.is_dir():
        print(f"FAIL  no game binaries at {GAME_BINARIES}")
        return 1

    # ── Which storefront is installed? Checking the wrong one is 100% false positives. ────────────
    storefronts = {
        p.name.split("-")[0]
        for p in GAME_BINARIES.glob("FactoryGame*-Win64-Shipping.exe")
    }
    if not storefronts:
        print(f"FAIL  no FactoryGame*-Win64-Shipping.exe in {GAME_BINARIES} - cannot tell storefront")
        return 1
    storefront = sorted(storefronts)[0]

    exe = GAME_BINARIES / f"{storefront}-Win64-Shipping.exe"
    build = "unknown"
    try:
        import subprocess

        build = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"(Get-Item '{exe}').VersionInfo.ProductVersion"],
            capture_output=True, text=True, timeout=30,
        ).stdout.strip() or "unknown"
    except Exception:
        pass

    print(f"game build : {build}")
    print(f"storefront : {storefront}")

    # ── Export universe: the game's modules AND the mods (they link against each other). ──────────
    # A module whose exports could not be READ is recorded as None and later reported NOT CHECKED.
    # It must never collapse into "exports nothing", which reads as "every import from it is missing".
    # ⚠ SCAN THE WHOLE GAME TREE, NOT ONE DIRECTORY. `Engine/Binaries/Win64` holds the 184 engine
    # modules, but `FactoryGame/Binaries/Win64` holds the GAME's own modules - including
    # `FactoryGameSteam-FactoryGame-Win64-Shipping.dll`, which is where AFGBuildable lives and which
    # is therefore the single most important module for mod linkage. Indexing only the engine
    # directory made every import from it fall through the "not a game module, the OS resolves it"
    # branch, and the gate PASSED a stack containing a mod observed failing to load minutes earlier.
    # Engine plugins each carry their own Binaries/Win64 too.
    universe: dict[str, set[bytes] | None] = {}
    game_dlls = [p for p in GAME_ROOT.rglob("*.dll") if MODS_ROOT not in p.parents]

    # ⚠ EVERY module DLL is named "FactoryGameSteam-<Module>-...", INCLUDING the engine ones, so the
    # filename prefix cannot tell a game module from an engine module. The install PATH can: game
    # modules live under FactoryGame\, the engine under Engine\. Measured 2026-08-14 - a first
    # attempt keyed on the name counted 114 CoreUObject/Core/Json/Slate virtuals as game exposure,
    # which would have made the invariant below fail permanently and therefore be switched off.
    game_module_names = {p.name.lower() for p in game_dlls if "Engine" not in p.parts}
    for p in game_dlls:
        universe[p.name.lower()] = exports_of(p)
    universe[exe.name.lower()] = exports_of(exe)

    mod_dlls = sorted(MODS_ROOT.rglob("*.dll")) if MODS_ROOT.is_dir() else []
    for p in mod_dlls:
        got = exports_of(p)
        if got is None:
            universe.setdefault(p.name.lower(), None)
        else:
            cur = universe.get(p.name.lower())
            universe[p.name.lower()] = got if cur is None else (cur | got)

    print(f"exports    : {len(game_dlls)} game module(s) + {len(mod_dlls)} mod binary(ies) indexed")

    # ── ★ SELF-TEST: prove the reader works before trusting a single absence it reports. ──────────
    # The first version of this script reported 50 of 59 mods broken because its export reader was
    # silently truncated at 8192 names. These three symbols cannot be missing from a game that runs,
    # so if the reader cannot see them the reader is broken - and a broken reader must REFUSE, not
    # produce a list of mods to go uninstall.
    engine = universe.get(f"{storefront.lower()}-engine-win64-shipping.dll")
    canaries = [
        b"?Tick@AActor@@UEAAXM@Z",
        b"?GetWorld@AActor@@UEBAPEAVUWorld@@XZ",
        b"?MarkRenderStateDirty@UActorComponent@@QEAAXXZ",
    ]
    if engine is None:
        print("\nFAIL  could not read the engine module's exports at all - REFUSING to report.")
        return 1
    absent = [c.decode() for c in canaries if c not in engine]
    if absent:
        print(f"\nFAIL  self-test: {len(absent)} symbol(s) that MUST exist were not found in the "
              f"engine module ({len(engine):,} names read):")
        for a in absent:
            print(f"        {a}")
        print("      The export reader is broken, not the mods. REFUSING to report - fixing this")
        print("      script is the job, and any 'broken mod' list it produced would be fiction.")
        return 1
    print(f"self-test  : ok - {len(engine):,} engine export names, all 3 canary symbols present")

    # ── Check every mod binary for THIS storefront. ───────────────────────────────────────────────
    broken: dict[str, list[tuple[str, bytes]]] = {}
    clean: list[str] = []
    unreadable: set[str] = set()
    skipped_other_storefront = 0
    checked = 0
    checked_mods: set[str] = set()

    # ★ FPM's OWN vtable exposure - see the invariant check below for why this is collected.
    SELF_MOD = "FicsitsPerformanceManager"
    self_virtual: list[tuple[str, bytes]] = []

    for dll in mod_dlls:
        if dll.name.startswith("FactoryGame") and not dll.name.startswith(storefront):
            skipped_other_storefront += 1
            continue

        # The mod's name is its folder under Mods/ (or Mods/GameFeatures/).
        rel = dll.relative_to(MODS_ROOT).parts
        mod = rel[1] if rel[0] == "GameFeatures" and len(rel) > 1 else rel[0]

        checked += 1
        checked_mods.add(mod)
        missing: list[tuple[str, bytes]] = []
        for from_dll, syms in imports_of(dll).items():
            if from_dll not in universe:
                # ⚠ A `FactoryGame*` import source that is not in the universe is a module we FAILED
                # TO FIND, not a system DLL. Skipping it silently is what let a known-broken mod pass.
                if from_dll.startswith("factorygame"):
                    unreadable.add(f"{from_dll} (NOT FOUND in the game tree)")
                # Otherwise: kernel32, vcruntime, ... - the OS resolves these.
                continue
            known = universe[from_dll]
            if known is None:
                unreadable.add(from_dll)  # NOT CHECKED - never counted as resolved
                continue
            missing.extend((from_dll, s) for s in syms if s not in known)

            if mod == SELF_MOD and from_dll in game_module_names:
                # The access nibble sits right after the `@@` that ends the qualified name.
                # U = public virtual, M = protected virtual. Either means vtable exposure.
                for sym in syms:
                    at = sym.find(b"@@")
                    if at >= 0 and sym[at + 2:at + 3] in (b"U", b"M"):
                        self_virtual.append((from_dll, sym))

        if missing:
            broken.setdefault(mod, []).extend(missing)
        else:
            clean.append(mod)

    print(f"checked    : {checked} mod binary(ies)"
          + (f"  ({skipped_other_storefront} skipped: other storefront, NOT CHECKED)"
             if skipped_other_storefront else ""))
    # ★ SAY WHAT THIS GATE CANNOT SEE, EVERY RUN.
    #
    # Measured 2026-08-12 on Ant's stack: 148 installed mods, of which only 61 carry native code. The
    # other 87 - 58% - are CONTENT-ONLY, and a PE import table is the only thing this tool reads. It has
    # literally nothing to look at for them.
    #
    # That blind spot is not theoretical. BigStorageTank is content-only and its author reported water
    # no longer flowing through it on this build; the vanilla pipe classes are all still present, so the
    # break is in the mod's own Blueprint. This gate called that stack's content half clean by saying
    # nothing about it, which is exactly the "absence of coverage reads as a clean bill of health"
    # failure the header warns about for unreadable modules. Same defect, larger surface.
    #
    # A content-only mod cannot raise Entry Point Not Found - it has no imports to resolve - so this
    # gate's verdict is still the right answer to "will the game START". It is NOT the answer to "is the
    # stack healthy", and printing the ratio is what stops the two being confused.
    content_only = sorted(m for m in installed_mod_names() if m not in checked_mods)
    if content_only:
        total = len(content_only) + len(checked_mods)
        print(f"NOT CHECKED: {len(content_only)} of {total} installed mod(s) are CONTENT-ONLY (no DLL) - "
              f"this gate reads PE imports and is blind to them by construction. It answers 'will the "
              f"game START', not 'is the stack healthy'. Their breakage is asset/Blueprint-level and "
              f"shows up in play, not at load.")

    if unreadable:
        print(f"⚠ NOT CHECKED: {len(unreadable)} module(s) whose exports could not be read - imports "
              f"from these were SKIPPED, not passed: {', '.join(sorted(unreadable))}")

    # ══ FPM'S OWN INVARIANT: ZERO VIRTUAL REFERENCES INTO GAME MODULES ═══════════════════════════
    #
    # ★ THIS IS THE PROPERTY THAT SAVED US ON 2026-08-11, AND UNTIL NOW IT WAS ONLY AN INTENTION.
    #
    # That update knocked out 13 mods. FPM was not among them, because it reaches game classes ONLY
    # through non-virtual members, so a vtable reshuffle cannot touch it. For contrast, RefinedPower
    # carried 358 virtual references into FactoryGame and RSS 313 - both died.
    #
    # That is a consequence of design, not luck: hooks go through SML's SUBSCRIBE_ macros instead of
    # subclassing FactoryGame types, and private members are reached via AccessTransformers.ini
    # instead of header edits - the first two measures the official docs list under "Avoid Breaking
    # in the First Place".
    #
    # ⚠ NOTHING ENFORCED IT UNTIL NOW. One fix subclassing an FG type reintroduces the exposure, and
    # the bill does not arrive until the NEXT game update, months later, when nobody connects the
    # two. So it is a gate, asserted every run against the SAME parsed import tables the resolution
    # check uses - never a regex over binary strings, which is how the contrast numbers above were
    # produced and is not accurate enough to gate on.
    if SELF_MOD in checked_mods:
        if self_virtual:
            print()
            print(f"★ INVARIANT BROKEN: {SELF_MOD} now imports {len(self_virtual)} "
                  f"VIRTUAL symbol(s) from game modules. It previously imported none.")
            print("  Virtual imports mean vtable exposure: next time CSS reshuffles a base class")
            print("  this mod breaks the way 13 others did on 2026-08-11. Prefer a SUBSCRIBE_ hook")
            print("  or an AccessTransformers friend over subclassing a game type.")
            for from_dll, sym in self_virtual[:10]:
                print(f"    {readable(sym)}")
                print(f"      from {from_dll}")
            if len(self_virtual) > 10:
                print(f"    ... and {len(self_virtual) - 10} more")
        else:
            print(f"ok    invariant: {SELF_MOD} imports ZERO virtual symbols from game modules "
                  f"- no vtable exposure, so a base-class reshuffle cannot break it")
    else:
        # NOT CHECKED is never a pass - the same law this tool applies to everything else.
        print(f"⚠ NOT CHECKED: {SELF_MOD} was not among the checked binaries, so its "
              f"zero-virtual invariant was NOT verified. Is it deployed for this storefront?")

    if args.verbose and clean:
        print(f"\nRESOLVE CLEAN ({len(sorted(set(clean)))}):")
        for m in sorted(set(clean)):
            print(f"  ok  {m}")

    if not broken:
        print(f"\nRESULT: every checked mod resolves against {build}. Nothing here blocks a boot.")
        # A broken invariant is a FAILURE even when nothing is broken today - that is the point.
        return 1 if self_virtual else 0

    # ── Is it patchable? An access-level change is; a removed virtual is NOT. ────────────────────
    # MSVC encodes ACCESS into the mangled name, right after the `@@` ending the qualified name:
    #   U = public virtual   M = protected virtual   I = protected   Q = public   A/E = private
    # So `?Foo@AClass@@UEAAXXZ` -> `?Foo@AClass@@MEAAXXZ` means CSS made Foo protected and nothing
    # else changed: same function, same address, same vtable slot. Rewriting that one byte in a mod's
    # import name table would be a genuinely safe repair.
    #
    # ⚠ A SYMBOL WITH NO ACCESS-VARIANT IS A DIFFERENT AND FAR WORSE CASE - DO NOT STUB IT.
    # These are VIRTUAL functions. Removing a virtual from a base class shortens its vtable and
    # shifts every slot after it. A mod's subclass vtable was emitted against the OLD layout, so
    # stubbing the import to make the DLL load leaves the engine calling slot N by the NEW index into
    # a table built with the OLD one: the wrong function, silently, with no error anywhere.
    # ★ REFUSING TO LOAD IS THE SAFE FAILURE. A mod that will not load is a mod you KNOW is broken.
    # A mod patched into loading with a stale vtable is memory corruption wearing a working mod's
    # name - and it would reach a shared save and a dedicated server before anyone noticed.
    #
    # Tools that already exist (WinDepends, Dependency Walker, PE Explorer, dumpbin /EXPORTS) all
    # stop at "this import is missing". None of them decode the access nibble, which is the whole
    # decision - hence this, rather than a dependency.
    game_exports = universe.get(f"{storefront.lower()}-factorygame-win64-shipping.dll") or set()

    def access_variant(sym: bytes) -> bytes | None:
        """The same symbol at a different C++ access level, if the game still exports one."""
        at = sym.find(b"@@")
        if at < 0 or len(sym) <= at + 2:
            return None
        head, cur, tail = sym[: at + 2], sym[at + 2 : at + 3], sym[at + 3 :]
        for c in b"AEIMQU":
            alt = head + bytes([c]) + tail
            if bytes([c]) != cur and alt in game_exports:
                return alt
        return None

    verdict: dict[str, tuple[int, int]] = {}
    for mod, syms in broken.items():
        renamable = sum(1 for _, s in syms if access_variant(s))
        verdict[mod] = (renamable, len(syms) - renamable)

    print(f"\n★ {len(broken)} MOD(S) CANNOT LOAD ON THIS GAME BUILD:\n")
    for mod in sorted(broken):
        syms = broken[mod]
        renamable, gone = verdict[mod]
        tag = "RENAME-PATCHABLE" if gone == 0 else "NEEDS A REBUILD BY ITS AUTHOR"
        print(f"  {mod}  -  {len(syms)} unresolved import(s)   [{tag}]")
        for from_dll, sym in syms[:6]:
            alt = access_variant(sym)
            nice = readable(sym)
            print(f"      {sym.decode(errors='replace')}")
            if alt:
                print(f"        -> {nice}: access level only, {sym[-12:].decode()} -> "
                      f"{alt[-12:].decode()} (same function)")
            else:
                print(f"        -> {nice}: REMOVED - no variant at any access level")
        if len(syms) > 6:
            print(f"      ... and {len(syms) - 6} more")
        print()

    total_gone = sum(g for _, g in verdict.values())
    print("Each of these raises a modal 'Entry Point Not Found' dialog BEFORE the engine finishes")
    print("starting, so the game hangs with no window rather than reporting a mod error.\n")
    if total_gone:
        print("⚠ DO NOT BINARY-PATCH THE ONES MARKED 'NEEDS A REBUILD'. The missing symbols are")
        print("  VIRTUAL functions that no longer exist at any access level, so the base class's")
        print("  vtable changed shape. Stubbing the imports would make the DLL load while leaving")
        print("  the engine calling virtual slots by the new index into a table built for the old")
        print("  layout - the wrong function, silently. Refusing to load is the SAFE failure here.")
        print("  These need a rebuild from their authors against this game build.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
