# TOW art contract

This document tells the art side what the C++ reads. The C++ file is
`Source/FicsitsPerformanceManager/Public/Wrist/FPMTowItem.h`.

Written 2026-08-15. Every measurement in this document was taken on that date.

---

## 1. The device is a SKELETAL mesh

Ant ruled on 2026-08-15 that the hook SNAPS OPEN. The hook is a slim dart in flight. The flukes
open when the hook hits. A splayed hook looks wrong stowed and wrong in flight, but splayed is the
only shape that holds a load.

Two shapes of one object is one skeletal mesh with two poses. It is not a static mesh, and it is
not two meshes.

The C++ holds a `USkeletalMeshComponent`. It cannot show a static mesh.

---

## 2. The three mount point names

The C++ reads these three names. It reads no other name.

| Name | What attaches there |
|------|---------------------|
| `Mount` | The device pivot. It is the device's own origin. |
| `LineExit` | The point the rope leaves the drum. The rope lane reads this every frame. |
| `HookStow` | The place the hook parks when the device is stowed. |

The three strings are declared ONCE in the whole mod, as `AFPMTowItem::MountName`,
`AFPMTowItem::LineExitName` and `AFPMTowItem::HookStowName` in `FPMTowItem.h`. No other file holds a
copy. If a name changes, it changes in that one place.

Use the names exactly. Do not add a prefix. Do not change the letter case.

---

## 3. Author the three names as SKELETON SOCKETS

Author each of the three as a socket on the skeleton. A socket is the correct answer.

A BONE with the same name also works. The C++ accepts it, and the transform it gives is correct.
But a bone is not what this contract asks for, and the report says so:

- `SOCKET` means the art did what this document asks.
- `BONE (no socket authored)` means the art shipped a joint and authored no socket. It works.
- `NOT FOUND` means nothing can attach there.

### Why the `SOCKET_` prefix rule does not apply here

The current placeholder names its three empty nodes `SOCKET_Mount`, `SOCKET_LineExit` and
`SOCKET_HookStow`. That prefix belongs to the STATIC mesh import path. Unreal's importer strips the
prefix and makes a static mesh socket from the remainder.

This device is a skeletal mesh, so that path does not run. On a skeletal mesh the mount points must
be skeleton sockets, or bones. The prefix does nothing.

---

## 4. The two pose frames

Author ONE animation sequence. Do not author two.

- Frame 0 is the CLOSED pose. The hook is a slim dart. The flukes are folded.
- The LAST frame is the OPEN pose. The flukes are out. This is the shape that holds a load.

The C++ plays the sequence FORWARD to open the hook. It plays the SAME sequence at a NEGATIVE play
rate to close it. One sequence is enough, and it removes the risk that a separate closing animation
drifts away from the opening animation's first frame.

The sequence must have a play length above zero. A zero-length sequence makes the hook jump between
the two poses with no movement.

---

## 5. The current placeholder does not meet this contract yet

`TOW_placeholder_v1.glb`, 289236 bytes, glTF binary version 2. These numbers were read out of the
file's own JSON chunk on 2026-08-15:

| Measurement | Value |
|-------------|-------|
| Meshes | 26 |
| Mesh-bearing nodes | 26 |
| Mesh-less nodes | 3 (`SOCKET_Mount`, `SOCKET_LineExit`, `SOCKET_HookStow`) |
| Scene root nodes | 29 |
| Nodes with children | 0 |
| Skins | 0 |
| Animations | 0 |

An earlier note said this file holds about 111 mesh parts. That number is wrong. The count is 26.

Two problems follow from the table, and both are art-side:

1. **Zero skins means zero skeleton.** A glTF with no skin cannot import as a skeletal mesh. This
   file must be re-exported with a skeleton before any of the rest matters.
2. **All 29 nodes sit flat at the scene root.** No node has a child. The three socket nodes are not
   below any mesh, so they would be dropped even if the file did import.

The C++ does not assume either problem is solved. It reports what it finds. Run `FPM.Tow.Report` in
the game console to see the current state.

---

## 6. OPEN QUESTION FOR ANT: which snap mechanism

The concept sheet shows two mechanisms. Ant chooses. This document names both and chooses neither.

- **Swept flukes.** Each fluke turns outward from the shaft. The flukes stay flat against the shaft
  when the hook is closed.
- **An opening X.** A cross form opens on a single axis. The four arms move together.

Both are one skeletal mesh with two poses, so the C++ is the same either way. The choice changes
the bone count and the silhouette, not the code.

A second question sits behind it, and it is a scope question rather than a design one: if the hook
belongs to the MODULE and the module is the upgrade unit, then each upgrade tier can carry a
different hook. That is more story for free, and it is more animation to author.

---

## 7. Where the imported assets must land

The C++ points at these two paths. Nothing exists at either of them today.

| Asset | Path |
|-------|------|
| Skeletal mesh | `/FicsitsPerformanceManager/Wrist/TOW/SK_TOW_Placeholder` |
| Deploy sequence | `/FicsitsPerformanceManager/Wrist/TOW/AS_TOW_HookDeploy` |

Both are soft references. The mod loads neither of them at start up, so a missing asset costs
nothing and prints no error at boot.

---

## 8. What must be true before the item is turned on

`FFPMTowItemHook::DefaultArmed()` returns false today, so the item registers nothing. Turn it on
only after all three of these are true, in this order:

1. The mesh imports as a skeletal mesh, with the three mount points authored as sockets.
2. The mesh and the sequence sit at the two paths in section 7.
3. `FPM.Tow.Report` states that all three mount points resolved, AND states that its known-positive
   classifier check ran instead of reporting it as not exercised.

Ship no vanilla asset and no third-party asset. Only art that NOX owns may ship.

## Socket convention (v16, 2026-08-15)

All three sockets are children of V10_Hull and live in the TOW_v10 collection.
All three share one axis convention: socket +Z points down the bore, along world +Y.
Positions, world mm: SOCKET_Mount (0, 0, 0). SOCKET_LineExit (0, 240, 64). SOCKET_HookStow (0, 182, 64).
SOCKET_HookStow marks the frame-20 stowed hook root. The stow action Hook_Stow retracts Root by 103.5 mm along -Y.
Do not re-guess these. Change them only with a matching change to the Hook_Stow action.


## v17 state (2026-08-15, round 3)

This section supersedes the v16 socket convention section above.

### Socket reference positions, world mm, frame 1
- SOCKET_Mount (0, 0, 0). Unchanged.
- SOCKET_LineExit (0, 240, 64). Unchanged.
- SOCKET_HookStow (-0.8, 150.8, 65.2). Moved from (0, 182, 64). It now marks the measured centre of the stowed hook group at frame 20.

The hook rest pose changed. TOW_Hook_Rig object location moved -57.5 mm in Y. At frame 1 the Hook_Eye min Y is 238.0, seated at the muzzle mouth. The Hook_Stow action still retracts Root by 103.5 mm along -Y from that seat.

Section 1 rules the device is one skeletal mesh. Therefore the three names must be authored as SKELETON SOCKETS at export. The three SOCKET_ empties in the file are position references only. A skeletal import drops them.

### Arm proxy stations, locked
The REF_Arm rings in the file are the proxy of record. Stations along +Y from ELBOW_Station at y0:
y 0, 36, 72, 108, 144, 180, 216, 252 mm. Radius 50.94 mm at y0, linear taper to 36.06 mm at y252.
Formula: r(y) = 50.94 - 0.059048 * y, in mm.
No version of this arm has been measured from game data. Every clearance number in the round 3 report is proxy relative. To replace the proxy, measure the pioneer forearm from the FModel dump in 20-SOURCES and rebuild the rings, then re-run the conformity sweep.

### Rope
Rope_Core is a visual stub only. It spans the muzzle mouth to the seated Hook_Eye, y 238.5 to 242.8 at z 64, bevel 3.5 mm. The in game rope is a runtime cable driven from LineExit. Exclude the TOW_Line collection from the export set.

### Texture route
Decision: tiling material route. The shipping meshes keep flat PBR materials (NOX_Steel, TOW_Body, TOW_Gold, TOW_Accent, NOX_Emissive). Per object UV islands are box projections at mixed density. If a bake pass is ever commissioned, every shipping mesh must first be packed into one shared 0 to 1 atlas at a single target density. Until then no unique bake is possible and none is planned.

### Open question for Ant, carried from round 2
Dart or hook. The hook set ships. The six V10_Dart_ objects were moved to the TOW_Dart_Pending collection, excluded from the view layer. The ten Dart_ objects remain hidden in TOW_Line. If Ant keeps the hook, delete both dart sets. If Ant wants the dart, it needs a rebuild, because V10_Dart_Fluke_0 is degenerate.

### Cutters
The five boolean cutters plus the new V10_DishCut live in the TOW_Cutters collection, excluded from the view layer. The hull booleans still evaluate. Do not export TOW_Cutters.
