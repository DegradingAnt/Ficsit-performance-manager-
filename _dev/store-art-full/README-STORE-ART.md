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
| NOX_Store_Banner_2400x536.png | 2400×536 | store page header |
| NOX_Card_Thumbnail_640x360.png | 640×360 | mod card / social preview |

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
