# Store art — ficsit.app listing

All PNGs, final pixel sizes, exported from the locked tile mark (NOX + corrupted O with the
FICSIT check knockout, orange framing brackets, "NOTICE OF EXCESS" serial rule).

## Mod logo / store icon
| File | Size | Use |
|---|---|---|
| NOX_Mod_Logo_1024.png | 1024×1024 | master — upload this to the ficsit.app listing |
| NOX_Mod_Logo_512.png | 512×512 | high-dpi listing / press |
| NOX_Mod_Logo_256.png | 256×256 | listing thumbnail |
| NOX_Mod_Logo_128.png | 128×128 | **in-game / mod manager icon** — same file as ../Resources/Icon128.png |
| NOX_Mod_Logo_64.png | 64×64 | small UI slot |
| NOX_Mod_Logo_Dark_512.png | 512×512 | flat #17181B variant, orange top-line |
| NOX_Mod_Logo_Retro_512.png | 512×512 | alt variant |

Resources/Icon128.png is the UE plugin icon and is byte-identical to NOX_Mod_Logo_128.png.

## Page art
| File | Size | Use |
|---|---|---|
| NOX_Store_Banner_2400x536.png | 2400×536 | store page header — ⚠ **HOLD, see below** |
| NOX_Card_Thumbnail_640x360.png | 640×360 | mod card / social preview / README header. Clean, makes no claims. |

### ⚠ The 2400×536 banner carries two strings that describe FPM1, not this build

Found 2026-08-10 while adding it to the README. Both need a designer fix before the banner is used
anywhere public. The art itself is correct — only the text is wrong.

| Element on the banner | Problem | Correct for this build |
|---|---|---|
| The chip reading **"Client-side"** | Wrong, and it is the kind of wrong that costs a support thread. FPM sets `RequiredOnRemote: true`, arms on a dedicated server, and most of its fixes are server-authoritative. A player reading "client-side" will install it on the client alone and be refused the join. | **"Client + server"** |
| The tagline **"> Your settings are the floor. Everything above it is paid for out of frames you were throwing away."** | Describes the performance **governor**. This build has no governor. | Something that matches what ships: repairs, guards and diagnostics. |

Until both are corrected, use `NOX_Card_Thumbnail_640x360.png` for headers. It carries the mark, the
serial and the mod name, and it makes no claim that can go stale.

## Vector + geometry (in-game)
Under ../assets/logo-export/:
- svg/ — NOX_Mark_O.svg, NOX_N.svg, NOX_X.svg, NOX_Wordmark.svg, NOX_Lockup_Serial.svg
- vertices/NOX_geometry.json — letterform vertex data for procedural/UMG draw
- sdf/ — signed-distance masks for the O, check, and wordmark (crisp at any scale)
- masks/NOX_Check_KnockoutMask.png — the check knockout used inside the O
- layers/ — N, O, X as separate PNG layers for animation

## Colour
Ember in all store + in-game art is #E59344 (the shipped vanilla widget tint).
The brighter ficsit.app web orange #FA9549 is NOT used in these files.
