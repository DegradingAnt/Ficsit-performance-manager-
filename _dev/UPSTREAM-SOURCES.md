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
