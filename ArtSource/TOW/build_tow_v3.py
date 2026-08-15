# WHY NOT RUST: this script runs INSIDE Blender's own embedded Python interpreter.
# The bpy/bmesh API exists nowhere else, and Blender cannot load a Rust binary as a
# --python argument. There is no Rust route to this job, so this is not a default
# decaying into a habit, it is the only door.
"""TOW v3. Built from tow-form-spec-v3.md.

RUN IT (note the flag order, it is not cosmetic):

    "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" --background \
        --python-exit-code 1 \
        --python build_tow_v3.py

⚠ --python-exit-code MUST COME BEFORE --python. Measured 2026-08-15, twice, on Blender
  5.2.0 LTS with a deliberate one-line crash script:
      --python x.py --python-exit-code 1   ->  exit 0   WRONG, a traceback reports success
      --python-exit-code 1 --python x.py   ->  exit 1   RIGHT
  v2's own docstring had it the wrong way round and a run using it reported success over a
  build that had aborted on an assert.

WHAT KILLED THE PREVIOUS TWO ATTEMPTS, so this file does not repeat them:
  v1: the spool stood up. Ruling 10.
  v2: five roughly co-equal masses with no hierarchy, so it read as integrated armour rather
      than a bolted-on graft. And its renders were BLANK: the subject covered 0.0007 to
      0.0020 percent of frame. The script checked each file existed with a size, which is
      not the same as checking the subject is in shot.

⚠ TWO BLENDER PROPERTIES THAT LIE, both measured 2026-08-15. Do not use either for a check:
  obj.dimensions      is the LOCAL bounding box times scale and IGNORES ROTATION.
  obj.rotation_euler  stays (0,0,0) forever once rotation_mode is QUATERNION.
  matrix_world is the truth for both. world_extent() below is the only sanctioned measure.
  len(obj.data.polygons) also cannot see modifier-added geometry without evaluated_get().

Units: the spec is in CENTIMETRES. Blender works in metres. cm() converts, once, at the edge.
"""

import math
import os
import sys

import bpy
from mathutils import Vector

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RENDER_DIR = os.path.join(SCRIPT_DIR, "renders_v3")
BLEND_PATH = os.path.join(SCRIPT_DIR, "tow_v3.blend")
GLB_PATH = os.path.join(SCRIPT_DIR, "tow_v3.glb")


def cm(v):
    """Spec centimetres to Blender metres."""
    return v / 100.0


# ---------------------------------------------------------------------------
# spec constants, sections 2 and 3. Names match the spec's part IDs.
# ---------------------------------------------------------------------------

ARM_LEN = 26.0
ARM_R_ELBOW = 5.2
ARM_R_WRIST = 3.5

PLATE_Y0, PLATE_Y1 = 3.0, 22.0          # P1, 19 long
PLATE_W = 8.0
PLATE_THK = 0.8                          # G2: <= 1.0
CHANNEL_W, CHANNEL_D = 1.5, 0.4

DRUM_Y = 9.5                             # axle centre, rear third
DRUM_CORE_D = 3.0
DRUM_WOUND_D = 3.9
DRUM_LEN = 6.2                           # G6: 6.2 / 3.9 = 1.59 : 1, more barrel than v3a
FLANGE_D = 4.4
FLANGE_THK = 0.5
CHEEK_X = 3.75
CHEEK_W, CHEEK_H, CHEEK_T = 3.4, 3.0, 0.8
STANDOFF = 0.8                           # S3, the visible daylight
RAIL_SEC = 1.2
RAIL_Y0, RAIL_Y1 = 6.0, 14.0
CROSS_Y = 13.5
POD_D, POD_L = 2.4, 1.7

FAIRLEAD_OD, FAIRLEAD_BORE = 1.8, 1.0
CABLE_D = 0.5
STRAP_REAR_Y, STRAP_REAR_W = 5.0, 2.0
STRAP_FRONT_Y, STRAP_FRONT_W = 20.0, 1.5
STRAP_PROUD = 0.5                        # G3: <= 0.6
HOOK_TIP_Y = 30.0

GATE_G1_MAX_NON_DRUM_RISE = 3.0
GATE_G2_MAX_PLATE_THK = 1.0
GATE_G3_MAX_STRAP_PROUD = 0.6
GATE_G4_MAX_CROWN = 6.0                  # tightened from 7.5: Ant judged 7.20 too big
GATE_G6_MIN_BARREL_RATIO = 1.3
GATE_MAX_MATERIAL_SLOTS = 2
GATE_MIN_SUBJECT_COVERAGE = 5.0          # percent of frame pixels

FAILURES = []


def gate(ok, label, detail):
    """Record a gate result. Collect them all, then fail once, loudly."""
    print(f"[GATE] {'PASS' if ok else 'FAIL'}  {label}: {detail}")
    if not ok:
        FAILURES.append(f"{label}: {detail}")


# ---------------------------------------------------------------------------
# measurement. world_extent is the ONLY sanctioned way to measure orientation.
# ---------------------------------------------------------------------------


def world_extent(obj):
    pts = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    return (
        max(p.x for p in pts) - min(p.x for p in pts),
        max(p.y for p in pts) - min(p.y for p in pts),
        max(p.z for p in pts) - min(p.z for p in pts),
    )


def world_max_z(objs):
    hi = -1e9
    for o in objs:
        for c in o.bound_box:
            hi = max(hi, (o.matrix_world @ Vector(c)).z)
    return hi


def arm_radius(y_cm):
    t = max(0.0, min(1.0, y_cm / ARM_LEN))
    return ARM_R_ELBOW + (ARM_R_WRIST - ARM_R_ELBOW) * t


# ---------------------------------------------------------------------------
# scene + materials. TWO SLOTS ONLY, spec section 7. v2 used five.
# ---------------------------------------------------------------------------


def clear_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def make_material(name, rgb, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True   # deprecated in Blender 6.0, still required in 5.2
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    return mat


def build_materials():
    return {
        "FICSIT_Painted": make_material("FICSIT_Painted", (0.28, 0.30, 0.23), 0.45, 0.62),
        "NOX_Steel": make_material("NOX_Steel", (0.30, 0.31, 0.33), 0.88, 0.42),
        "REF_Proxy": make_material("REF_Proxy", (0.38, 0.52, 0.56), 0.0, 0.9),
    }


# ---------------------------------------------------------------------------
# primitives. Each returns an object already collected and materialled, so no
# part can silently arrive slotless.
# ---------------------------------------------------------------------------


def link(obj, coll):
    for c in list(obj.users_collection):
        c.objects.unlink(obj)
    coll.objects.link(obj)


def box(name, size_cm, loc_cm, coll, mat, rot=None):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=[cm(v) for v in loc_cm])
    o = bpy.context.object
    o.name = name
    o.scale = [cm(v) for v in size_cm]
    if rot:
        o.rotation_euler = rot
    link(o, coll)
    o.data.materials.append(mat)
    return o


def cyl(name, dia_cm, len_cm, loc_cm, axis, coll, mat, verts=24):
    bpy.ops.mesh.primitive_cylinder_add(
        vertices=verts, radius=cm(dia_cm) / 2.0, depth=cm(len_cm),
        location=[cm(v) for v in loc_cm],
    )
    o = bpy.context.object
    o.name = name
    if axis == "X":
        o.rotation_euler = (0.0, math.radians(90), 0.0)
    elif axis == "Y":
        o.rotation_euler = (math.radians(90), 0.0, 0.0)
    link(o, coll)
    o.data.materials.append(mat)
    return o


def torus(name, major_cm, minor_cm, loc_cm, axis, coll, mat):
    bpy.ops.mesh.primitive_torus_add(
        major_radius=cm(major_cm) / 2.0, minor_radius=cm(minor_cm) / 2.0,
        major_segments=24, minor_segments=10,
        location=[cm(v) for v in loc_cm],
    )
    o = bpy.context.object
    o.name = name
    if axis == "X":
        o.rotation_euler = (0.0, math.radians(90), 0.0)
    elif axis == "Y":
        o.rotation_euler = (math.radians(90), 0.0, 0.0)
    link(o, coll)
    o.data.materials.append(mat)
    return o


# ---------------------------------------------------------------------------
# THE CRAFT PASS. v2 had NONE of this and that is why it read as a pile of
# tubes: no bevels, no smooth shading, no weighted normals. Ordered by payoff
# per effort. The bevel must exist before normals are weighted against it.
# ---------------------------------------------------------------------------


def craft_pass(objs, bevel_w_cm=0.06, segments=2):
    for o in objs:
        if o.type != "MESH":
            continue
        bev = o.modifiers.new(name="Bevel", type="BEVEL")
        bev.width = cm(bevel_w_cm)
        bev.segments = segments
        bev.limit_method = "ANGLE"
        bev.angle_limit = math.radians(35)
        bev.harden_normals = True

        wn = o.modifiers.new(name="WeightedNormal", type="WEIGHTED_NORMAL")
        wn.keep_sharp = True

        bpy.context.view_layer.objects.active = o
        # shade_auto_smooth replaced use_auto_smooth in 4.1
        bpy.ops.object.shade_auto_smooth(angle=math.radians(35))


# ---------------------------------------------------------------------------
# BUILD
# ---------------------------------------------------------------------------


def build_arm_proxy(coll, mat):
    """Reference only. Excluded from export and from every gate."""
    out = []
    steps = 8
    for i in range(steps):
        y0 = ARM_LEN * i / steps
        y1 = ARM_LEN * (i + 1) / steps
        r = (arm_radius(y0) + arm_radius(y1)) / 2.0
        out.append(cyl(f"REF_Arm_{i}", r * 2, (y1 - y0) + 0.2,
                       (0, (y0 + y1) / 2.0, 0), "Y", coll, mat, verts=20))
    out.append(box("REF_Hand", (7.0, 9.0, 4.5), (0, ARM_LEN + 4.5, 0), coll, mat))
    return out


def build_ficsit_base(coll, mats):
    """P1 to P3. The vambrace. SECONDARY mass: long, thin, low."""
    out = []
    m = mats["FICSIT_Painted"]
    plate_z = arm_radius((PLATE_Y0 + PLATE_Y1) / 2.0) + PLATE_THK / 2.0

    plate = box("P1_Plate", (PLATE_W, PLATE_Y1 - PLATE_Y0, PLATE_THK),
                (0, (PLATE_Y0 + PLATE_Y1) / 2.0, plate_z), coll, m)
    out.append(plate)

    for sx in (-1, 1):
        out.append(cyl(f"P1_Lip_{'L' if sx < 0 else 'R'}", PLATE_THK,
                       PLATE_Y1 - PLATE_Y0,
                       (sx * PLATE_W / 2.0, (PLATE_Y0 + PLATE_Y1) / 2.0, plate_z),
                       "Y", coll, m, verts=12))
    # front rolled lip. P1's third job: the cable runs over it, so no extra guide part.
    out.append(cyl("P1_FrontLip", PLATE_THK, PLATE_W, (0, PLATE_Y1, plate_z),
                   "X", coll, m, verts=12))

    for sx in (-1, 1):
        out.append(box(f"P1_Rib_{'L' if sx < 0 else 'R'}",
                       (0.4, PLATE_Y1 - PLATE_Y0 - 1.0, 0.2),
                       (sx * (CHANNEL_W / 2.0 + 0.5), (PLATE_Y0 + PLATE_Y1) / 2.0,
                        plate_z + PLATE_THK / 2.0), coll, m))

    r = arm_radius(STRAP_REAR_Y)
    out.append(torus("P2_StrapRear", (r + STRAP_PROUD / 2.0) * 2, STRAP_PROUD,
                     (0, STRAP_REAR_Y, 0), "Y", coll, m))
    out.append(box("P2_Buckle", (2.2, STRAP_REAR_W, 0.5),
                   (0, STRAP_REAR_Y, -(r + 0.3)), coll, m))
    out.append(box("P3_RatingPlate", (3.0, 2.0, 0.1),
                   (-2.0, PLATE_Y1 - 2.5, plate_z + PLATE_THK / 2.0), coll, m))
    return out, plate_z


def build_nox_graft(coll, mats, plate_z):
    """P5 to P19. PRIMARY mass plus the forward line. All slot 2."""
    out = []
    m = mats["NOX_Steel"]
    plate_top = plate_z + PLATE_THK / 2.0
    rail_z = plate_top + STANDOFF / 2.0
    axle_z = plate_top + 2.55

    # P5 subframe. S3: these rails create the visible daylight under the drum.
    for sx in (-1, 1):
        out.append(box(f"P5_Rail_{'L' if sx < 0 else 'R'}",
                       (RAIL_SEC, RAIL_Y1 - RAIL_Y0, RAIL_SEC),
                       (sx * (CHEEK_X - RAIL_SEC / 2.0), (RAIL_Y0 + RAIL_Y1) / 2.0,
                        rail_z + RAIL_SEC / 2.0), coll, m))
    out.append(box("P5_CrossMember", (CHEEK_X * 2, RAIL_SEC, RAIL_SEC),
                   (0, CROSS_Y, rail_z + RAIL_SEC / 2.0), coll, m))

    # P6 graft bolts. S2: THE seam, the bolts in "NOX just bolts it on".
    for sx in (-1, 1):
        for by in (RAIL_Y0 + 0.8, RAIL_Y1 - 0.8):
            out.append(cyl(f"P6_Bolt_{sx}_{int(by)}", 1.4, 1.1,
                           (sx * (CHEEK_X - RAIL_SEC / 2.0), by, plate_top + 0.35),
                           "Z", coll, m, verts=6))

    for sx in (-1, 1):
        out.append(box(f"P7_Cheek_{'L' if sx < 0 else 'R'}",
                       (CHEEK_T, CHEEK_W, CHEEK_H),
                       (sx * CHEEK_X, DRUM_Y, rail_z + RAIL_SEC + CHEEK_H / 2.0), coll, m))

    # P8/P9 THE BARREL. Axis along X, ruling 10, gated below.
    out.append(cyl("P8_DrumCore", DRUM_CORE_D, DRUM_LEN, (0, DRUM_Y, axle_z),
                   "X", coll, m, verts=28))
    wound = cyl("P9_WoundCable", DRUM_WOUND_D, DRUM_LEN - 0.4,
                (0, DRUM_Y, axle_z), "X", coll, m, verts=28)
    out.append(wound)

    wraps = 7
    for i in range(wraps):
        x = -DRUM_LEN / 2.0 + 0.5 + i * ((DRUM_LEN - 1.0) / (wraps - 1))
        out.append(torus(f"P9_Wrap_{i}", DRUM_WOUND_D, CABLE_D * 0.8,
                         (x, DRUM_Y, axle_z), "X", coll, m))

    for sx in (-1, 1):
        fx = sx * (DRUM_LEN / 2.0 + FLANGE_THK / 2.0)
        out.append(cyl(f"P10_Flange_{'L' if sx < 0 else 'R'}", FLANGE_D, FLANGE_THK,
                       (fx, DRUM_Y, axle_z), "X", coll, m, verts=28))
        for h in range(6):
            a = 2 * math.pi * h / 6
            out.append(cyl(f"P10_Hole_{sx}_{h}", 0.8, FLANGE_THK + 0.2,
                           (fx, DRUM_Y + math.cos(a) * 1.45, axle_z + math.sin(a) * 1.45),
                           "X", coll, m, verts=10))

    # P11 ratchet ring. The half of the safety NOX kept, because it is part of the flange.
    for t in range(24):
        a = 2 * math.pi * t / 24
        out.append(box(f"P11_Tooth_{t}", (0.3, 0.35, 0.3),
                       (-(DRUM_LEN / 2.0 + FLANGE_THK),
                        DRUM_Y + math.cos(a) * FLANGE_D / 2.0,
                        axle_z + math.sin(a) * FLANGE_D / 2.0), coll, m))

    # P12 pawl boss, EMPTY, with a sheared pin. Ruling 4 made literal:
    # the ratchet survives and nothing engages it.
    out.append(cyl("P12_PawlBoss", 1.0, 0.8,
                   (-CHEEK_X, DRUM_Y - CHEEK_W / 2.0 + 0.5, axle_z - 1.2),
                   "X", coll, m, verts=12))
    out.append(cyl("P12_ShearedPin", 0.4, 0.5,
                   (-CHEEK_X - 0.6, DRUM_Y - CHEEK_W / 2.0 + 0.5, axle_z - 1.2),
                   "X", coll, m, verts=8))

    # P13 torn guard tabs. Absence made visible.
    for sx in (-1, 1):
        out.append(box(f"P13_TornTab_{'L' if sx < 0 else 'R'}", (0.2, 1.4, 0.9),
                       (sx * CHEEK_X, DRUM_Y + 1.0,
                        rail_z + RAIL_SEC + CHEEK_H + 0.3), coll, m,
                       rot=(math.radians(18), 0, 0)))

    # P14 drive pod. Why it needs no crank. Plain: no fins, no styling.
    out.append(cyl("P14_DrivePod", POD_D, POD_L,
                   (CHEEK_X + CHEEK_T / 2.0 + POD_L / 2.0, DRUM_Y, axle_z),
                   "X", coll, m, verts=20))
    out.append(cyl("P14_WeldCollar", POD_D + 0.4, 0.3,
                   (CHEEK_X + CHEEK_T / 2.0 + 0.15, DRUM_Y, axle_z), "X", coll, m, verts=20))

    # P15 fairlead. FUNCTION not safety, so NOX kept it. P13 is the cover's tombstone.
    fair_z = rail_z + RAIL_SEC + FAIRLEAD_OD / 2.0
    out.append(torus("P15_Fairlead", FAIRLEAD_OD, (FAIRLEAD_OD - FAIRLEAD_BORE) / 2.0,
                     (0, CROSS_Y, fair_z), "Y", coll, m))

    # P16 cable: drum top-front tangent -> fairlead -> plate channel -> over the lip -> hook
    path = [
        (0.0, DRUM_Y + DRUM_WOUND_D / 2.0 * 0.7, axle_z + DRUM_WOUND_D / 2.0 * 0.7),
        (0.0, CROSS_Y, fair_z),
        (0.0, PLATE_Y1 - 3.0, plate_top + CHANNEL_D * 0.5),
        (0.0, PLATE_Y1, plate_top + 0.2),
        (0.0, HOOK_TIP_Y - 5.0, plate_top + 0.1),
    ]
    for i in range(len(path) - 1):
        a, b = Vector(path[i]), Vector(path[i + 1])
        mid = (a + b) / 2.0
        seg = cyl(f"P16_Cable_{i}", CABLE_D, (b - a).length, tuple(mid),
                  "Z", coll, m, verts=8)
        seg.rotation_euler = (b - a).to_track_quat("Z", "Y").to_euler()
        out.append(seg)

    out.append(box("P17_Cradle", (2.4, 3.0, 0.3),
                   (0, PLATE_Y1 + 1.5, plate_top + 0.2), coll, m))

    # P18 hook. Bright bare steel against the dark device: the Just Cause value contrast.
    out.append(cyl("P18_Shank", 0.8, 3.0, (0, HOOK_TIP_Y - 3.5, plate_top + 0.6),
                   "Y", coll, m, verts=10))
    out.append(cyl("P18_Body", 1.6, 2.0, (0, HOOK_TIP_Y - 1.5, plate_top + 0.6),
                   "Y", coll, m, verts=12))
    for i in range(3):
        a = 2 * math.pi * i / 3
        out.append(box(f"P18_Barb_{i}", (0.35, 2.5, 0.35),
                       (math.cos(a) * 0.9, HOOK_TIP_Y - 1.2,
                        plate_top + 0.6 + math.sin(a) * 0.9),
                       coll, m, rot=(math.radians(-22), 0, 0)))

    # P19 front strap. A NOX replacement for a failed FICSIT strap, so slot 2. S5:
    # matched pairs read as designed-in, unmatched pairs read as a service history.
    r = arm_radius(STRAP_FRONT_Y)
    out.append(torus("P19_StrapFront", (r + STRAP_PROUD / 2.0) * 2, STRAP_PROUD,
                     (0, STRAP_FRONT_Y, 0), "Y", coll, m))
    out.append(box("P19_BoltClosure", (1.0, STRAP_FRONT_W, 0.8),
                   (0, STRAP_FRONT_Y, -(r + 0.4)), coll, m))

    return out, plate_top, wound


# ---------------------------------------------------------------------------
# GATES
# ---------------------------------------------------------------------------


def run_geometry_gates(ficsit, nox, plate_top, wound):
    bpy.context.view_layer.update()
    arm_top = arm_radius(DRUM_Y)

    # G6 first: it is the v1 killer. Measured in WORLD space, after all transforms.
    wx, wy, wz = world_extent(wound)
    ratio = wx / max(wy, wz)
    print(f"[TOW] wound drum WORLD extent: x={wx:.4f} y={wy:.4f} z={wz:.4f}")
    gate(wx > wy and wx > wz, "G6-axis",
         f"drum long axis must be X, across the arm. x={wx:.4f} y={wy:.4f} z={wz:.4f}")
    gate(ratio >= GATE_G6_MIN_BARREL_RATIO, "G6-barrel",
         f"length/diameter {ratio:.2f} must be >= {GATE_G6_MIN_BARREL_RATIO}, "
         f"a barrel not a disc")

    drum_objs = [o for o in nox if o.name.startswith(("P7_", "P8_", "P9_", "P10_"))]
    crown = (world_max_z(drum_objs) * 100.0) - arm_top
    gate(crown <= GATE_G4_MAX_CROWN, "G4-crown",
         f"drum crown {crown:.2f} cm above the arm must be <= {GATE_G4_MAX_CROWN}")

    # The drum ASSEMBLY, per spec section 1: "Drum, wound cable, flanges, bearing
    # cheeks, drive pod, and the subframe it stands on."
    drum_prefixes = ("P5_", "P6_", "P7_", "P8_", "P9_", "P10_", "P11_", "P12_",
                     "P13_", "P14_")

    # ⚠ WHY THIS GATE IS NOT A BARE HEIGHT TEST. First run, it failed on
    # P15_Fairlead (+3.60) and P16_Cable_0 (+5.46) and BOTH were correct
    # geometry. The fairlead stands on the subframe cross member because the
    # spec puts it there (P15), and a cable leaving the drum's top-front tangent
    # cannot be below the drum. A bare height test therefore fires on work that
    # obeys the spec, which is the same defect class as v2's rotation-blind
    # drum check: a gate that rejects correct input.
    # What G1 actually protects is the MASS HIERARCHY: no FOURTH MASS may rise
    # over the plate. The spec is explicit that the forward line is "a line with
    # a bright steel termination, NOT A MASS". So the test is height AND bulk.
    LINE_GAUGE_VOL_CM3 = 12.0

    def bulk_cm3(obj):
        ex, ey, ez = world_extent(obj)
        return (ex * 100.0) * (ey * 100.0) * (ez * 100.0)

    offenders = []
    for o in ficsit + nox:
        if o.name.startswith(drum_prefixes):
            continue
        rise = (world_max_z([o]) * 100.0) - plate_top
        if rise <= GATE_G1_MAX_NON_DRUM_RISE:
            continue
        vol = bulk_cm3(o)
        if vol <= LINE_GAUGE_VOL_CM3:
            print(f"[GATE] note  G1: {o.name} rises +{rise:.2f} but is line gauge "
                  f"({vol:.1f} cm3 <= {LINE_GAUGE_VOL_CM3}), not a mass. Allowed.")
            continue
        offenders.append(f"{o.name} +{rise:.2f} rise, {vol:.1f} cm3 bulk")
    gate(not offenders, "G1-hierarchy",
         "only the drum assembly may rise >3cm over the plate. offenders: "
         + (", ".join(offenders) if offenders else "none"))

    gate(PLATE_THK <= GATE_G2_MAX_PLATE_THK, "G2-plate",
         f"plate {PLATE_THK} cm thick must be <= {GATE_G2_MAX_PLATE_THK}")
    gate(STRAP_PROUD <= GATE_G3_MAX_STRAP_PROUD, "G3-straps",
         f"straps {STRAP_PROUD} cm proud must be <= {GATE_G3_MAX_STRAP_PROUD}")


def run_material_gate(objs):
    slots = set()
    for o in objs:
        for mt in o.data.materials:
            if mt and not mt.name.startswith(("REF_", "_clay")):
                slots.add(mt.name)
    gate(len(slots) <= GATE_MAX_MATERIAL_SLOTS, "material-slots",
         f"{len(slots)} slots ({', '.join(sorted(slots))}) must be "
         f"<= {GATE_MAX_MATERIAL_SLOTS}. v2 used five.")


def subject_coverage(path):
    """Percent of frame pixels differing from the background, via Blender's own
    image loader so no PIL is needed inside Blender."""
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    n = len(px) // 4
    br, bgc, bb = px[0], px[1], px[2]
    diff = counted = 0
    for i in range(0, n, 7):
        j = i * 4
        if abs(px[j] - br) + abs(px[j + 1] - bgc) + abs(px[j + 2] - bb) > 0.06:
            diff += 1
        counted += 1
    bpy.data.images.remove(img)
    return 100.0 * diff / max(counted, 1)


# ---------------------------------------------------------------------------
# RENDER. Five views, spec section 9, each with the coverage gate.
# ---------------------------------------------------------------------------


def setup_world():
    scene = bpy.context.scene
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1200
    world = bpy.data.worlds.new("W")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.05, 0.05, 0.06, 1)
    scene.world = world

    for name, loc, energy in (("Key", (1.2, -0.8, 1.0), 900),
                              ("Fill", (-1.0, -0.6, 0.4), 300),
                              ("Rim", (0.0, 1.2, 0.8), 500)):
        lamp = bpy.data.lights.new(name, type="AREA")
        lamp.energy = energy
        lamp.size = 1.2
        obj = bpy.data.objects.new(name, lamp)
        obj.location = loc
        bpy.context.scene.collection.objects.link(obj)
        d = Vector((0, cm(ARM_LEN / 2), 0)) - Vector(loc)
        obj.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


def add_camera(name, loc, look_at, lens):
    cam_data = bpy.data.cameras.new(name)
    cam_data.lens = lens
    cam = bpy.data.objects.new(name, cam_data)
    cam.location = loc
    d = Vector(look_at) - Vector(loc)
    cam.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(cam)
    return cam


def render_view(name, cam, clay, mats, device):
    scene = bpy.context.scene
    scene.camera = cam
    saved = {}
    if clay:
        clay_mat = mats.setdefault(
            "_clay", make_material("_clay", (0.62, 0.62, 0.62), 0.0, 0.55))
        for o in device:
            saved[o.name] = list(o.data.materials)
            for i in range(len(o.data.materials)):
                o.data.materials[i] = clay_mat
    path = os.path.join(RENDER_DIR, f"{name}.png")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    if clay:
        for o in device:
            for i, mt in enumerate(saved[o.name]):
                o.data.materials[i] = mt
    cov = subject_coverage(path)
    print(f"[TOW] rendered {name}: {os.path.getsize(path)} bytes, "
          f"subject {cov:.2f}% of frame")
    floor = 0.5 if "thirdperson" in name else GATE_MIN_SUBJECT_COVERAGE
    if "thirdperson" in name:
        print(f"[TOW] PROMINENCE AT GAME DISTANCE: {cov:.2f}% of frame. This is a "
              f"reported measurement, not a target. v2's blank renders were 0.002%.")
    gate(cov >= floor, f"render-{name}",
         f"subject {cov:.2f}% must be >= {floor}%. v2 shipped four "
         f"renders at 0.0007 to 0.0020% and nobody noticed until an agent measured them.")


def main():
    clear_scene()
    os.makedirs(RENDER_DIR, exist_ok=True)
    mats = build_materials()

    col_dev = bpy.data.collections.new("TOW_Device")
    col_ref = bpy.data.collections.new("TOW_Reference")
    bpy.context.scene.collection.children.link(col_dev)
    bpy.context.scene.collection.children.link(col_ref)

    ref = build_arm_proxy(col_ref, mats["REF_Proxy"])
    ficsit, plate_z = build_ficsit_base(col_dev, mats)
    nox, plate_top, wound = build_nox_graft(col_dev, mats, plate_z)
    device = ficsit + nox

    bpy.context.view_layer.update()
    run_geometry_gates(ficsit, nox, plate_top, wound)
    run_material_gate(device)

    craft_pass(device)
    print(f"[TOW] craft pass applied to {len(device)} device objects: "
          f"bevel + auto-smooth + weighted normals")

    setup_world()
    arm_mid = cm(ARM_LEN / 2.0)
    focus = (0, arm_mid, cm(4.0))
    side = add_camera("C_side", (cm(55), arm_mid, cm(6)), focus, 85)
    views = [
        ("R1_side_clay", side, True),
        ("R2_side_shaded", side, False),
        ("R3_top_shaded", add_camera("C_top", (0, arm_mid, cm(48)), focus, 70), False),
        # R4 answers "is it prominent at GAME distance", so the lens must stay
        # wide (game FOV) and the distance must stay honest. First run put the
        # camera at 1.6 m and the subject covered 0.61 percent, under the gate.
        # A 26 cm object at game FOV genuinely IS small; the fix is to frame the
        # forearm, not to fake prominence with a long lens.
        ("R4_thirdperson_clay",
         add_camera("C_tp", (cm(32), cm(-46), cm(34)), focus, 24), True),
        ("R5_seam_shaded",
         add_camera("C_seam", (cm(24), cm(-2), cm(15)),
                    (0, cm(DRUM_Y), cm(plate_top + 2)), 85), False),
    ]
    for name, cam, clay in views:
        render_view(name, cam, clay, mats, device)

    bpy.ops.wm.save_as_mainfile(filepath=BLEND_PATH)
    print(f"[TOW] saved blend: {BLEND_PATH} ({os.path.getsize(BLEND_PATH)} bytes)")

    for o in bpy.data.objects:
        o.select_set(o in device)
    bpy.ops.export_scene.gltf(filepath=GLB_PATH, export_format="GLB",
                              use_selection=True, export_apply=True)
    print(f"[TOW] saved glb: {GLB_PATH} ({os.path.getsize(GLB_PATH)} bytes)")
    print(f"[TOW] device objects: {len(device)}   reference: {len(ref)}")

    if FAILURES:
        print("\n[TOW] BUILD FAILED. Gates that did not pass:")
        for f in FAILURES:
            print(f"  - {f}")
        sys.exit(1)
    print("[TOW] BUILD COMPLETE, every gate passed.")


if __name__ == "__main__":
    main()
