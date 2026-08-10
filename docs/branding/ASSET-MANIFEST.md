# NOX — Asset manifest & UE5 build guide

Every asset the mod needs, labelled, with **UE5 import settings** and **how to make it in engine**.
Three classes — read the class first:

- **SHIP** — NOX-original art, packaged in the mod `.pak`. Listed with import + build notes.
- **REFERENCE (vanilla)** — Satisfactory's own textures. **Never packaged.** Loaded from the
  running game by object path at runtime. The mockups copy them in only to look right.
- **RETIRE** — do not ship, do not reference; superseded.

Colours referenced below are in `tokens.json` (sRGB + UE-linear). Palette proof: `assets/vanilla-palette.json`.

---

## Provenance — what may ship

Three classes, and they are not interchangeable:

| Class | What it is | Ship rule |
|---|---|---|
| **vanilla** | textures extracted from Satisfactory (ticks, warning glyph, speedometer, corruption frames, pad glyphs, monochrome icons, HUD/visor art, ADA marks, in-game captures) | **never redistribute.** They live here so the mocks read true. In engine, borrow them at runtime through **`UFPMVanillaArt`** (the helper already exists — do not write a second one, and do not copy the texture into our content). The game object paths are listed in "Vanilla assets used in mocks" below. |
| **vanilla-derived** | NOX art that embeds or traces vanilla geometry — anything with the FICSIT check punched through it (all wordmarks, lockups, O-marks, icon tiles, the knockout mask, the tick mask, the SDF set) | **rebuild in engine:** our ember disc + the game's checkmark texture as the knockout mask (`M_NOX_OMark`). The flattened PNGs here are references and mock art, not shipping textures. |
| **NOX-original** | our own geometry only — the N and X letterforms, their vector/vertex sources, the retired glyphs (9 files) | ships freely. |
| **screen capture** | frames grabbed from the running game (`game-scene.png`, `scene-a/b.png`, `ref-menu.png`) | mock backdrop only. **Not an asset** — never ship in any form. |

Classification is an **allowlist**: only paths matching an explicit NOX-original rule are labelled ours. Anything unmatched is treated as vanilla. The audit page reports zero unclassified files; if that count is ever non-zero, classify before shipping.

Fonts are third-party: Open Sans is the vanilla UI face (already in-game, do not package); IBM Plex Mono is OFL and **must** be packaged.

`NOX Asset Audit.dc.html` renders this classification over the live file list — every asset, its reference count, and its ship rule on one page.

## Canon assets — one asset per UI element

Same element ⇒ same asset, on every screen. Do not hand-build these.

| UI element | Canon asset | Notes |
|---|---|---|
| Tick / checkmark (checkbox, window title, emblem) | `FICSIT_Checkmark.png` | the one vanilla path (`…/Assets/Shared/ficsit_checkmark_256`). `icons/ficsit_check_ui.png` was a duplicate export and is **retired** |
| Tintable tick (needs a colour other than white/black) | `icons/glyph/NOX_Tick_Mask.png` | luminance mask — tint in the material |
| Compact NOX mark (HUD box title, readout chip, server chip, "Managed by NOX" tag) | `assets/logo-export/NOX_Mark_O_Ember.png` | never an ember disc + tick assembled in widget code, and never bare `NOX_Mark_O.png` (not a shipped variant) |
| Full wordmark (window headers, wizard, hero, store) | `nox-logo.js` → `WBP_NOX_Logo` | `size` ≥ 16; below that use the O-mark |
| Wordmark + subtitle | `NOX_Lockup_Serial_*.png` or `nox-logo serial="true"` | one rule + tracked "NOTICE OF EXCESS" |
| Warning glyph (tags, severity chips) | `icons/vanilla-warning-icon.png` | `…/HUD_Elements/HudBoxIcons/warning_icon` |
| Bench gauge | `icons/vanilla-speedometer.png` | vanilla 0–150 arc |
| Corruption overlay | `icons/vanilla-corruption-0{0,1,2}.png` | vanilla tear frames |
| Controller hints | `icons/pad_{a,b,x,y,lb,rb,dpad}.png` (Xbox) · `icons/ps_{cross,circle,square,triangle,l1,r1,dpad}.png` (PS5) | vanilla glyph sets — pick by the active input device, never mix. Mapping: A↔✕, B↔○, X↔□, Y↔△, LB↔L1, RB↔R1 |

## SHIP — NOX-original assets

### Logo / marks — `assets/logo-export/`

| File | Purpose | Dims | Alpha |
|---|---|---|---|
| `NOX_Wordmark_Steel.png` | primary wordmark, steel letters + ember O (light/paper) | 1088×440 | yes |
| `NOX_Wordmark_White.png` | wordmark for dark HUD (white letters + ember O) | 1088×440 | yes |
| `NOX_Wordmark_Ember.png` / `_Mono.png` / `_Critical.png` | all-ember / all-white / hazard-red variants | 1088×440 | yes |
| `NOX_Lockup_Serial_*.png` | wordmark + "NOTICE OF EXCESS" rule (5 colour variants) | 816×435 | yes |
| `NOX_Mark_O_Ember.png` / `_White.png` / `_Critical.png` / `_Mono.png` | the O-mark solo (disc + knockout check). `_Mono` is retired grey `#8C8C8C` — the dead/stopped state (kill switch, oversight off) | 512×512 | yes |
| `NOX_Icon_Tile.png` | store/app icon — dark tile, ember corner brackets, glow, knockout O | 512×512 | yes |
| `svg/NOX_{N,X,Mark_O,Wordmark,Wordmark_White,Lockup_Serial}.svg` | **vector sources** (scale to any HUD size) | vector | — |
| `vertices/NOX_geometry.json` | exact N/X polygons + O circle + layout metrics for procedural/Blueprint draw | data | — |
| `masks/NOX_Check_KnockoutMask.png` | white field / black check — luminance mask for the O knockout | 512×512 | — |
| `sdf/NOX_SDF_{Wordmark,Mark_O,Check}.png` | **signed-distance fields** (mid-grey = edge, spread 14px) — crisp via a UE SDF material: `smoothstep(0.5-w, 0.5+w, R)` into opacity, tint by param. **Caveat: good up to ~2× native res** (all HUD/menu sizes); beyond that corners soften — use the SVG sources or the 1088px PNG masters for hero/store sizes. See `sdf/_SDF_Preview.png` (880px shows the limit) | 512×202 / 256² / 256² | grey |
| `sdf/NOX_SDF_{Wordmark_Hero2048,Mark_O_Hero1024,Check_Hero1024}.png` | **hero-res SDF set** — same encoding, spread scales with res (56px @2048 / 28px @1024); covers store/hero decodes to ~4K, so the 2× caveat above only applies to the 512 HUD set | 2048×829 / 1024² / 1024² | grey |

### Glyphs — `icons/glyph/`

| File | Purpose | Dims | Alpha |
|---|---|---|---|
| ~~`NOX_Managed_Diamond`~~ | **RETIRED** — the MANAGED tag now leads with the compact NOX O-mark (`M_NOX_OMark` at 11–12 px) | — | — |
| ~~`NOX_Close`~~ | **RETIRED** — window close uses the font `×` U+00D7 (in Open Sans) | — | — |
| `NOX_Tick_Mask.png` | `✓` as a **white/alpha mask** — tint per use (ember on fills, `--nox-signal` on scene ticks) | 256² | mask |

### UE import settings

| Asset kind | Texture Group | Compression | sRGB | Mips | Notes |
|---|---|---|---|---|---|
| Colour logos (wordmarks, tile) | `UI` | `UserInterface2D` (BC7) | **on** | NoMipmaps | Filter Bilinear; "Never Stream" for HUD marks. |
| Alpha masks (glyphs, tick, knockout) | `UI` | `Alpha`/`Grayscale` | **off** | NoMipmaps | Single channel; tint in UMG via `SetColorAndOpacity`. |
| SVG sources | — | — | — | — | UE 5.x imports SVG as vector; keep for authoring/SDF, or rasterize per-DPI. |

### How to make each in engine

- **Wordmark / lockup** — import the PNG (colour, UI group). For infinite scale, import the SVG or
  bake an SDF from `NOX_Wordmark_White.png`. Recolour by tinting a white master in UMG.
- **O-mark** — two routes:
  1. **Texture:** import `NOX_Mark_O_*.png` (or the white master + tint).
  2. **Material `M_NOX_OMark` (scales forever):** filled circle (SDF) **minus** `NOX_Tick_Mask`
     sampled at 54% centred (knockout) → `OneMinus` the mask into opacity. Param `EmberColor`.
     Use this for any HUD size; recolour by param (ember / `--nox-hazard` on critical).
- **Corrupted O** (ships — compact / badge, holds at 16px) — `M_NOX_OMark` + a horizontal-tear
  pass: offset a thin UV band, add chromatic split; the tear heals→corrupts on a cycle and hits
  harder on the deception/anger spike (see `NOX-VOICE-AND-CORRUPTION.md`).
- **Aperture eye** (ships — oversight moments) — a small `WBP`: dark radial lens, ember iris ring,
  `NOX_Tick_Mask` pupil, and a **stuttering aperture spin that skips only when it corrupts** + a
  chromatic tear flick. Geometry + the JS corruption-coupling are in `NOX Decisions.dc.html`.
  (The Audit-gauge direction was cut.)
- **Glyphs `◆ ✕ ✓`** — import the mask PNG, draw as an `Image` with `SetColorAndOpacity`. The SVGs
  (`◆ ✕`) are there if you prefer vector.
- **N / X letterforms** — pure polygons; `vertices/NOX_geometry.json` gives the exact vertex lists
  for `MakeCustomVerts` / a procedural Blueprint, or use the SVGs.

---

## Fonts — `fonts/`

| Font | Role | Ship? |
|---|---|---|
| **Open Sans** 400/600/700 | all UI + NOX's *forced* ADA-register lines | vanilla UI face — already in-game; do not package |
| **IBM Plex Mono** 400/600 | NOX's own voice (`>` lines, terminal, ledger) | **must be packaged** — OFL, redistributable. Not yet in this repo; drop the OFL TTFs in `fonts/` and import as a UE Font asset. |

See `NOX-VOICE-AND-CORRUPTION.md` for the two-register rule.

---

## 4K / resolution rules

- **Author once, scale with DPI.** CSS px in the mockups map 1:1 to UMG slate units at 1080p.
  Let **UMG DPI scaling** handle 1440p/4K — do **not** hard-scale widgets. Curve: `1920→1.0`,
  `2560→1.33`, `3840→2.0` (Custom-Scaling in Project Settings › User Interface).
- **UI textures:** import at native size, `UserInterface2D`, **NoMipmaps** — they stay crisp
  scaling *down*, so author generously. The shipped logos (1088-wide wordmark, 512² marks) cover
  up to 4K HUD use. If a mark is drawn larger than its native px at 4K, use the **SVG or the
  `M_NOX_OMark` material** instead of the PNG (vector = no blur at any resolution).
- **Prefer vector/material for anything that scales with the HUD** (marks, gauge, meter) so 4K is
  free; reserve PNGs for fixed-size chrome.
- **Per-screen mock canvas sizes (for 4K checks):** every menu screen (S1–S3, S6, S7, S8) and the
  in-game HUD are authored on a **1920×1080 stage** (scaled to fit in the mock); the oversight
  readout chip is 250–290 px wide at 1080p. At 4K everything is exactly **×2** via the DPI curve —
  never re-author.
- Minimum legibility still applies: HUD readout text ≥ the vanilla `Widget_HUDBox` font sizes,
  scaled by the DPI curve.

---

## REFERENCE (vanilla) — never packaged, load by game object path

The impl side already resolved 34 vanilla textures byte-identical; these are the ones the NOX
work touches. Load at runtime; do **not** redistribute.

| Mock file | Vanilla object path |
|---|---|
| `FICSIT_Checkmark.png` / tick | `/Game/FactoryGame/Interface/UI/Assets/Shared/ficsit_checkmark_256` |
| `icons/MIcon_Cogwheel.png` (`⚙`) | `…/UI/Assets/MonochromeIcons/TXUI_MIcon_Cogwheel` |
| `icons/MIcon_ArrowRight.png` (`→`) | `…/UI/Assets/MonochromeIcons/TXUI_MIcon_Arrow_Right` |
| `icons/vanilla-warning-icon.png` | `…/UI/Assets/HUD_Elements/HudBoxIcons/warning_icon` |
| `icons/vanilla-speedometer.png` | `…/UI/Assets/Shared/Vehicle_Speedometer` (bench gauge, 0–150 arc) |
| `icons/vanilla-corruption-0{0,1,2}.png` | `…/UI/Assets/Shared/CorruptionImages/TXUI_Corruption_0{0,1,2}` (voice/UI corruption) |
| fuse red / amber (semantic) | `…/UI/Assets/Shared/Fusebox/Fusebox_{Off,On}Light` (sampled `#EE441A` / `#F69B2B`) |
| `icons/ada-logo.png` / `ada-speechbubble.png` | `…/UI/Message/TXUI_ADALogo` / `TXUI_ADALogo_SpeechBubble` (ADA-pipeline ref only) |
| `hud/visor_*`, `compass_*`, `bar_health_*`, `icon_health` | `…/UI/Assets/HUD_Elements/…` (HUD overlay backdrop in mocks) |
| controller pad glyphs (`pad_*`) | `…/UI/Assets/Gamepad/ButtonXSX*` — **runtime via `FGButtonHintBar`** so they switch per device; never ship fixed art |

---

## Popups (S5) — building blocks only, no shipped mock

The popup mocks in `design/` are **reference only — do not replicate them 1:1**. Assemble S5 in
engine from these blocks: the **vanilla ADA pipeline** (`Widget_Subtitle` + `BPW_ADANotification`,
inherited at runtime), **IBM Plex Mono** for his lines / **Open Sans** for the forced register,
the **corruption engine** (`NOX-VOICE-AND-CORRUPTION.md` — rates, deception spike, audio),
**severity colours** from `tokens.json` (`color.ingame`), **`M_NOX_OMark`** for any NOX mark, and
the caption-strip geometry in `HANDOFF-UE5.md §S5`.

## RETIRE — do not ship or reference

- `icons/meter-glass.png`, `icons/meter-indicator.png` — replaced by the vanilla speedometer gauge.
- `icons/ficsit_check_ui.png` — **retired**; every mock now references `FICSIT_Checkmark.png`; for a tintable tick use `icons/glyph/NOX_Tick_Mask.png`.
- `game-scene.png`, `scene-a.png`, `scene-b.png` — mockup backdrops only; real screens composite
  over the live game.


## Screens are canon too

The 15 `.dc.html` design files are themselves canon artefacts — each is a build target, not a sketch. `NOX Asset Audit.dc.html` lists them with status:

- **canon** (11) — Mod UI (S1/S2/S3/S6/S7), Quick Bench (S8), Debug View (D1–D3), HUD States (S4b), Settings Pages (S9/S10), Benchmark Passes (B1–B3), Console (S11), Server Overlay (S12), Overrule (S13/S14), Store Hero (out-of-game).
- **part canon** (1) — In Game: S4 readout is canon; **S5 popups are reference only**, assembled in engine from the shipped blocks.
- **reference** (4) — Screens index, Decisions, Asset Audit, Logo Pass.
