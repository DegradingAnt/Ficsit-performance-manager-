# UPSTREAM SOURCES — where each third-party mod ACTUALLY develops, and what we may do with it

Created 2026-08-15, first use of `sf-upstream-source`. Same convention as `DEPENDENCY-LEDGER.md`.

**Why this file exists.** A GitHub 404 on a mod that develops on GitLab reads as "no update", and a
missing licence reads as "go ahead" if nobody wrote down that it was missing. Both are silent false
negatives. Record the host and the licence once, here, so the next pass reads instead of re-deriving —
and so a **read-only** finding can never be promoted into shipped code by accident.

**How to read the LICENCE column.** Per `sf-mine` step 0, the licence is checked in four places, in
order: the repo `LICENSE` file · the `.uplugin` metadata · the ficsit.app mod page · the README footer.
**Absent from all four means NO, not maybe** — no declared licence is all rights reserved by default.

## Entries

```
Efficiency Checker Mod — ficsit:EfficiencyCheckerMod · host:github
  repo:https://github.com/MarcioHuser/EfficiencyCheckerMod-SML3 · branch:master
  licence:GPL-3.0 (LICENSE.txt, confirmed via GitHub licence API) · confirmed:2026-08-15 · confidence:high
  last upstream push: 2026-06-14 — BEFORE 1.2.4.0 broke it. Author has not rebuilt.

Power Checker — ficsit:PowerChecker · host:github
  repo:https://github.com/marcioHuser/powerchecker-SML3 · branch:master
  licence:GPL-3.0 (LICENSE.txt, confirmed via GitHub licence API) · confirmed:2026-08-15 · confidence:high
  last upstream push: 2026-06-12 — BEFORE 1.2.4.0 broke it. Author has not rebuilt.

Cartograph: Buildings on Map — ficsit:Cartograph · host:github
  repo:https://github.com/yeshjho/Cartograph · branch:Cartograph
  licence:GPL-3.0 (LICENSE.txt, confirmed via GitHub licence API) · confirmed:2026-08-15 · confidence:high
  ⚠ TWO URLS NAME THIS MOD AND THEY DISAGREE. SMR source_url says yeshjho/Cartograph; the shipped
  .uplugin CreatedByURL says yeshjho/SatisfactoryMod/tree/Cartograph. Both resolve, both report the
  SAME pushed_at (2026-06-11T12:33:17Z), the same default branch (Cartograph) and the same licence —
  so they are one repo under two names, not a stale mirror. Either is safe to read.
  ★ WE ALREADY MAINTAIN A PATCHED FORK (m5788937, 3 bugs, shipped to client + server + SunFry).
  Source lives OUTSIDE the FPM repo, per the precedent.

MarcioCommonLibs — ficsit:MarcioCommonLibs · host:github
  repo:https://github.com/MarcioHuser/MarcioCommonLibs-SML3 · branch:master
  ⛔ licence:NONE DECLARED — ALL RIGHTS RESERVED · confirmed:2026-08-15 · confidence:high
  ★ Checked by CLONING the repo and searching the full tree, not by trusting an API or a filename —
  `find . -iname "*licen*" -o -iname "*copying*" -o -iname "readme*"` returns NOTHING (the command
  itself was sanity-checked against this same clone first, so the empty result is a real absence, not
  a broken search). `MarcioCommonLibs.uplugin` carries no licence field at all, only `CreatedBy`/
  `CreatedByURL`/`DocsURL`, and `SupportURL` is the empty string. Same author as EfficiencyCheckerMod
  and PowerChecker (both GPL-3.0, confirmed below) — the omission reads as an oversight, not a
  deliberate reservation, but an oversight is not a licence.
  ⚠ CORRECTS AN EARLIER ERROR: this repo was previously banked as "GPL-3.0, same as the other two
  MarcioHuser repos" without being checked on its own. It is not. Each repo's licence is its own fact
  and must be verified on its own, even when a sibling repo under the same author checks out clean.
  It is needed only as a BUILD-TIME HEADER DEPENDENCY for the two GPL mods above (the installed copy
  ships 0 `.h` files, so this clone is the only source of headers) — reading its headers to compile
  against them is not the same as copying or redistributing its code, so building the two GPL mods
  locally stays defensible. ⛔ DO NOT REDISTRIBUTE a rebuilt MarcioCommonLibs binary. If a future game
  update breaks MCL itself, the fix is NOT ours to make and ship: ask the author to add a licence line
  (cheapest — the PR conversation for the two GPL rebuilds already needs to reach him), or the two
  dependent mods fall with it, or an M-CARRY vanilla-surface guard is designed the same way as the
  HeavyFluids entry below. REGISTERED, not admitted to M-CARRY — no live MCL bug exists today (R3
  §15.2 / assembled design §9.14).

Heavy Fluid Overhaul — ficsit:HeavyFluids · host:NONE
  repo:none, ficsit-only · branch:n/a
  ⛔ licence:NONE DECLARED — ALL RIGHTS RESERVED · confirmed:2026-08-15 · confidence:high
  Checked all four places 2026-08-15 and found nothing: SMR source_url is the empty string; the
  shipped .uplugin carries only CreatedBy (Acxd, SirDigby, Delektrix) and a Discord SupportURL; the
  ficsit.app page's full_description is 4,844 chars with ZERO licence, copyright, fork or reuse
  language; there is no repo, so there is no README to have a footer.
  ⛔ DO NOT PATCH, FORK, OR MODIFY. Read-only. The only route is a bug report to the authors via
  their Discord (https://discord.gg/pz2U6FvMat), and only with Ant's approval.
```

## Method note, so the next reader trusts these

Host and licence were **not** taken from the ficsit.app page text. `source_url` came from the SMR
GraphQL API (`getModByReference`), and the licence from GitHub's own licence endpoint
(`/repos/{owner}/{repo}/license`), which reports the SPDX id it detected from the actual file rather
than from a claim in a README. `archived` and `fork` were checked on every repo and are false
throughout, so none of these is a dead mirror.

⚠ **A licence verdict is about REDISTRIBUTION as well as reading.** GPL-3.0 permits modifying and
redistributing, provided the source ships and the licence is preserved — FPM is GPL-3.0 itself, so
that is compatible. "No licence" does not merely restrict shipping a fork; it means the code may be
read but not copied or modified for use.

## Correction pass, 2026-08-15 (later the same day)

All four entries above were **re-verified against the local clones at `C:/Modding/forks/`**, not
re-trusted from the GitHub API — reading the actual `LICENSE.txt` bytes (confirming real GPL-3.0
license text, not just a filename or an API's SPDX guess) for the three GPL entries, and a full
recursive filename search plus a read of `MarcioCommonLibs.uplugin` for the fourth. This is the
stronger check the earlier pass should have run: an API's "no licence" or "licence: X" answer is
itself a claim, not a fact, and the project has already been burned once by trusting a probe over
a directory listing (a `LICENSE.md` that turned out to be a directory, elsewhere in this project's
history). **MarcioCommonLibs was added by this pass** — it exists in the same author's repo family as
two confirmed-GPL mods and was previously assumed to share their licence without being checked on its
own. It does not. Nothing else in the four-entry table changed: EfficiencyCheckerMod, PowerChecker,
and Cartograph all check out GPL-3.0 by direct content read, and HeavyFluids' no-repo/no-licence
finding is unchanged and was not re-checked further today (there is no repo to clone).
