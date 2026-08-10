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

⚠ **Neither page-art file is in this folder, and neither may be used yet.** Both are placeholders
built over a screen capture of the running game. See below.

| File | Size | Status |
|---|---|---|
| NOX_Store_Banner_2400x536.png | 2400×536 | **REMOVED** — game capture backdrop, plus two wrong strings |
| NOX_Card_Thumbnail_640x360.png | 640×360 | **REMOVED** — game capture backdrop |

Use `NOX_Mod_Logo_1024.png` for the listing and `NOX_Mod_Logo_512.png` or the Dark variant for a
README header. Those are the tile mark on a drawn panel, with no capture behind them.

### Why both were removed

The manifest in this folder already states the rule. Under *"Provenance — what may ship"*, the
**screen capture** class reads:

> frames grabbed from the running game (`game-scene.png`, `scene-a/b.png`, `ref-menu.png`) — mock
> backdrop only. **Not an asset** — never ship in any form.

Both page-art files are that backdrop with the lockup composited on top. Ant, 2026-08-10, on the card
thumbnail: *"you cant use that. it has a fucking screenshot of the game behind the art, its a
placeholder."*

⚠ They were committed to the public repo before this was caught, so they remain reachable in git
history even though they are gone from the working tree.

### And the banner also carries two strings that describe FPM1

Separate from the capture problem, and still true when the art is redone:

| Element | Problem | Correct for this build |
|---|---|---|
| The chip reading **"Client-side"** | The reverse of true, and the kind of wrong that costs a support thread. FPM sets `RequiredOnRemote: true`, arms on a dedicated server, and most of its fixes are server-authoritative. A player who reads "client-side" installs it on the client alone and is refused the join. | **"Client + server"** |
| The tagline **"> Your settings are the floor. Everything above it is paid for out of frames you were throwing away."** | Describes the performance **governor**. This build has no governor. | Something that matches what ships: repairs, guards and diagnostics. |

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
