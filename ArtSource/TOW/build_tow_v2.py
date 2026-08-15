# WHY NOT RUST: this script runs INSIDE Blender's own embedded Python
# interpreter via `blender --background --python`, driving the `bpy` mesh,
# material, camera and glTF-export API. That API is Python-only -- Blender
# has no Rust bindings for its scripting/rendering pipeline, so there is no
# Rust path to this at all, not even a harder one.
"""
TOW prototype v2 -- forearm-mounted grapple device, built procedurally.

Re-runnable headless build script. Run with:
  "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" --background \
      --python build_tow_v2.py --python-exit-code 1

Produces, next to this script:
  tow_v2.blend
  tow_v2.glb              (device mesh only -- forearm proxy excluded)
  renders/side.png
  renders/top.png
  renders/three_quarter.png
  renders/mounted.png

Design authority: FPM-GRAPPLE-VISUAL-BRIEF-2026-08-15.md. Every numbered
ruling cited in a comment below is from that brief. The single ruling that
sank v1 is ruling 10: the spool must lie down (horizontal axis) and run
ACROSS the arm, not along it. That is checked with a hard assertion below,
not just a comment, because "I meant to" is what shipped the wrong thing
last time.

Palette is a PLACEHOLDER. The brief's ruling 2/6 says the real palette
comes from the NOX brand tokens under 20-SOURCES/FPM-brand/, which is
outside this script's scope path -- the brief itself lists exact
palette/finish as left open on purpose, so a placeholder here is not a
gap, it is the brief's own boundary.
"""

import bpy
import math
import os
import struct
import sys
from mathutils import Vector

# ---------------------------------------------------------------------------
# constants -- all in metres (Blender scene units are set to metric, scale 1)
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RENDER_DIR = os.path.join(SCRIPT_DIR, "renders")
BLEND_PATH = os.path.join(SCRIPT_DIR, "tow_v2.blend")
GLB_PATH = os.path.join(SCRIPT_DIR, "tow_v2.glb")

# forearm proxy (research: avg forearm length ~25cm, circumference-derived
# radius ~4.8cm at the widest point, tapering toward the wrist)
FOREARM_LEN = 0.26
R_ELBOW = 0.052
R_WRIST = 0.035
HAND_LEN = 0.09
HAND_WIDTH = 0.07
HAND_HEIGHT = 0.045

# device housing (ruling 10: flush but not too flush -- stays inboard of
# both elbow and wrist, visible standoff, stepped panels read as the
# "uneven seams, coarser tolerances" of ruling 6)
DEV_Y_START = 0.03
DEV_Y_REAR_MID = 0.11
DEV_Y_MID_FRONT = 0.17
DEV_Y_END = 0.23
STANDOFF = 0.007
HOUSING_THK = 0.012

# drum / reel (ruling 10: horizontal axis, ACROSS the arm -- world X.
# ruling 3: prominent and mechanical, wound cable visible. positioned at
# the rear near the elbow, per the JC3 layout takeaway in ruling 5)
DRUM_Y = 0.075
DRUM_RADIUS = 0.026
DRUM_LEN = 0.062
FLANGE_RADIUS = 0.032
FLANGE_THK = 0.005
BEARING_BLOCK_W = 0.016
BEARING_BLOCK_H = 0.026
BEARING_BLOCK_D = 0.03
N_CABLE_WRAPS = 6
WRAP_BULGE = 0.0065

# barrel / hook (ruling 5: hook projects forward past the hand, bright
# bare steel against the dark body; ruling 1: fires over the back of the
# hand)
BARREL_RADIUS = 0.014
BARREL_LEN = 0.15
BARB_COUNT = 3

# cuffs (ruling 10: cuffs grip the arm, standoff to the housing)
CUFF_REAR_Y = 0.04
CUFF_FRONT_Y = 0.215
CUFF_GAP = 0.006
CUFF_THICKNESS = 0.006

MAT_DEFS = {
    # name: (base_color_rgb, metallic, roughness)
    "MAT_FICSIT_Base": ((0.30, 0.32, 0.24), 0.55, 0.6),
    "MAT_NOX_Addon": ((0.20, 0.24, 0.20), 0.3, 0.78),
    "MAT_Bare_Steel": ((0.62, 0.63, 0.65), 0.9, 0.32),
    "MAT_Rust_Accent": ((0.28, 0.16, 0.10), 0.2, 0.82),
    "MAT_Reference": ((0.35, 0.55, 0.60), 0.0, 0.9),
}

# ---------------------------------------------------------------------------
# scene setup
# ---------------------------------------------------------------------------


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for block_holder in (bpy.data.meshes, bpy.data.materials, bpy.data.cameras,
                         bpy.data.lights):
        for block in list(block_holder):
            if block.users == 0:
                block_holder.remove(block)


def setup_units():
    scene = bpy.context.scene
    scene.unit_settings.system = 'METRIC'
    scene.unit_settings.scale_length = 1.0
    scene.unit_settings.length_unit = 'METERS'


def make_materials():
    mats = {}
    for name, (color, metallic, roughness) in MAT_DEFS.items():
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        bsdf.inputs["Base Color"].default_value = (*color, 1.0)
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = roughness
        mats[name] = mat
    return mats


def make_collections():
    col_device = bpy.data.collections.new("TOW_Device")
    bpy.context.scene.collection.children.link(col_device)
    col_ref = bpy.data.collections.new("Scale_Reference")
    bpy.context.scene.collection.children.link(col_ref)
    return col_device, col_ref


def move_to_collection(obj, collection):
    for c in list(obj.users_collection):
        c.objects.unlink(obj)
    collection.objects.link(obj)


def _world_extent(obj):
    """World-space bounding-box extent. Rotation-aware, unlike obj.dimensions."""
    pts = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    return (
        max(p.x for p in pts) - min(p.x for p in pts),
        max(p.y for p in pts) - min(p.y for p in pts),
        max(p.z for p in pts) - min(p.z for p in pts),
    )


def align_to(obj, direction, up='X'):
    if direction.length < 1e-9:
        return
    d = direction.normalized()
    obj.rotation_mode = 'QUATERNION'
    obj.rotation_quaternion = d.to_track_quat('Z', up)


def arm_radius(y):
    t = max(0.0, min(1.0, y / FOREARM_LEN))
    return R_ELBOW + (R_WRIST - R_ELBOW) * t


# ---------------------------------------------------------------------------
# primitive helpers
# ---------------------------------------------------------------------------


def add_cylinder(name, radius, depth, location, direction=Vector((0, 0, 1)),
                  radius2=None, vertices=16, collection=None, material=None,
                  up='X'):
    bpy.ops.mesh.primitive_cone_add(
        vertices=vertices,
        radius1=radius,
        radius2=radius if radius2 is None else radius2,
        depth=depth,
        location=location,
    )
    obj = bpy.context.object
    obj.name = name
    align_to(obj, direction, up=up)
    if collection is not None:
        move_to_collection(obj, collection)
    if material is not None:
        obj.data.materials.append(material)
    return obj


def add_box(name, size, location, collection=None, material=None,
            direction=None, up='X'):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = size
    if direction is not None:
        align_to(obj, direction, up=up)
    if collection is not None:
        move_to_collection(obj, collection)
    if material is not None:
        obj.data.materials.append(material)
    return obj


def add_torus(name, major_radius, minor_radius, location, direction,
              major_segments=24, minor_segments=8, collection=None,
              material=None, up='X'):
    bpy.ops.mesh.primitive_torus_add(
        location=location,
        major_radius=major_radius,
        minor_radius=minor_radius,
        major_segments=major_segments,
        minor_segments=minor_segments,
    )
    obj = bpy.context.object
    obj.name = name
    align_to(obj, direction, up=up)
    if collection is not None:
        move_to_collection(obj, collection)
    if material is not None:
        obj.data.materials.append(material)
    return obj


# ---------------------------------------------------------------------------
# build: forearm proxy (scale reference, excluded from export)
# ---------------------------------------------------------------------------


def build_forearm_proxy(col_ref, mats):
    forearm = add_cylinder(
        "REF_Forearm", radius=R_ELBOW, radius2=R_WRIST, depth=FOREARM_LEN,
        location=(0, FOREARM_LEN / 2.0, 0), direction=Vector((0, 1, 0)),
        vertices=24, collection=col_ref, material=mats["MAT_Reference"],
    )
    hand = add_box(
        "REF_Hand", size=(HAND_WIDTH, HAND_LEN, HAND_HEIGHT),
        location=(0, FOREARM_LEN + HAND_LEN / 2.0, 0),
        collection=col_ref, material=mats["MAT_Reference"],
    )
    return forearm, hand


# ---------------------------------------------------------------------------
# build: housing plates (stepped -- uneven, NOX-era seams, ruling 6)
# ---------------------------------------------------------------------------


def build_housing(col_dev, mats):
    segments = [
        ("TOW_Housing_Rear", DEV_Y_START, DEV_Y_REAR_MID, 0.062),
        ("TOW_Housing_Mid", DEV_Y_REAR_MID, DEV_Y_MID_FRONT, 0.05),
        ("TOW_Housing_Front", DEV_Y_MID_FRONT, DEV_Y_END, 0.042),
    ]
    housings = {}
    bolts = []
    for name, y0, y1, width in segments:
        y_c = (y0 + y1) / 2.0
        r = arm_radius(y_c)
        z_bottom = r + STANDOFF
        z_c = z_bottom + HOUSING_THK / 2.0
        obj = add_box(
            name, size=(width, y1 - y0, HOUSING_THK), location=(0, y_c, z_c),
            collection=col_dev, material=mats["MAT_FICSIT_Base"],
        )
        housings[name] = obj
        z_top = z_bottom + HOUSING_THK
        for sx in (-1, 1):
            for sy in (-1, 1):
                bx = sx * (width / 2.0 - 0.006)
                by = y_c + sy * ((y1 - y0) / 2.0 - 0.008)
                idx = len(bolts)
                bolt = add_cylinder(
                    f"TOW_Bolt_{idx:02d}", radius=0.003, depth=0.005,
                    location=(bx, by, z_top + 0.0025),
                    direction=Vector((0, 0, 1)),
                    vertices=8, collection=col_dev,
                    material=mats["MAT_Bare_Steel"],
                )
                bolts.append(bolt)
    return housings, bolts


# ---------------------------------------------------------------------------
# build: the reel -- the ruling-10 critical part
# ---------------------------------------------------------------------------


def build_reel(col_dev, mats, rear_housing_top_z):
    axle_z = rear_housing_top_z + BEARING_BLOCK_H
    block_y = DRUM_Y
    for side, sx in (("L", -1), ("R", 1)):
        bx = sx * (DRUM_LEN / 2.0 + BEARING_BLOCK_W / 2.0)
        add_box(
            f"TOW_BearingBlock_{side}",
            size=(BEARING_BLOCK_W, BEARING_BLOCK_D, BEARING_BLOCK_H),
            location=(bx, block_y, rear_housing_top_z + BEARING_BLOCK_H / 2.0),
            collection=col_dev, material=mats["MAT_FICSIT_Base"],
        )

    drum = add_cylinder(
        "TOW_Drum", radius=DRUM_RADIUS, depth=DRUM_LEN,
        location=(0, DRUM_Y, axle_z), direction=Vector((1, 0, 0)),
        vertices=20, collection=col_dev, material=mats["MAT_Bare_Steel"],
    )

    # --- THE RULING-10 GATE: spool must be horizontal AND run across the
    # arm (world X), not standing up (tall in Z) and not running along the
    # arm (long in Y). This is checked in code, not just intended in a
    # comment, because v1 failed this exact ruling.
    bpy.context.view_layer.update()
    # ⚠ obj.dimensions is the LOCAL bounding box times scale and IGNORES ROTATION.
    # Reading it here made this gate FIRE ON CORRECT WORK: the drum was already
    # lying down on world X and the gate reported it standing up, because it was
    # measuring the unrotated extents. Measured 2026-08-15 with a two-case probe:
    #   obj.dimensions  x=0.0520 y=0.0520 z=0.0620
    #   WORLD bbox      x=0.0620 y=0.0520 z=0.0520   <- the truth
    # The world-space bounding box is the only reading that answers the question
    # this gate is actually asking.
    dx, dy, dz = _world_extent(drum)
    print(f"[TOW] drum WORLD extent (rotation-aware): x={dx:.4f} y={dy:.4f} z={dz:.4f}")
    assert dx > dz, (
        f"RULING 10 VIOLATION: drum barrel is not longer than it is tall -- "
        f"length x={dx:.4f} <= height z={dz:.4f}. The spool reads as standing "
        f"up like v1. Aborting."
    )
    assert dx > dy, (
        f"RULING 10 VIOLATION: drum axis is not running across the arm -- "
        f"x={dx:.4f} <= y={dy:.4f}. The spool axis is along the forearm "
        f"instead of across it. Aborting."
    )

    for side, sx in (("L", -1), ("R", 1)):
        fx = sx * DRUM_LEN / 2.0
        add_cylinder(
            f"TOW_Flange_{side}", radius=FLANGE_RADIUS, depth=FLANGE_THK,
            location=(fx, DRUM_Y, axle_z), direction=Vector((1, 0, 0)),
            vertices=24, collection=col_dev, material=mats["MAT_Bare_Steel"],
        )

    margin = FLANGE_THK + 0.006
    span = DRUM_LEN - 2 * margin
    for i in range(N_CABLE_WRAPS):
        t = i / (N_CABLE_WRAPS - 1) if N_CABLE_WRAPS > 1 else 0.5
        wx = -span / 2.0 + t * span
        add_torus(
            f"TOW_CableWrap_{i:02d}", major_radius=DRUM_RADIUS + WRAP_BULGE,
            minor_radius=WRAP_BULGE * 0.85,
            location=(wx, DRUM_Y, axle_z), direction=Vector((1, 0, 0)),
            major_segments=20, minor_segments=8, collection=col_dev,
            material=mats["MAT_Bare_Steel"],
        )

    # severed brake linkage (ruling 4): a stub axle beyond one bearing
    # block, cut short -- what used to carry a band brake.
    stub_x = -(DRUM_LEN / 2.0 + BEARING_BLOCK_W + 0.012)
    add_cylinder(
        "TOW_AxleStub_SeveredBrake", radius=0.0045, depth=0.02,
        location=(stub_x, DRUM_Y, axle_z), direction=Vector((1, 0, 0)),
        vertices=10, collection=col_dev, material=mats["MAT_Rust_Accent"],
    )

    return axle_z


# ---------------------------------------------------------------------------
# build: fairlead guide, NOX defeat details, barrel, hook barbs
# ---------------------------------------------------------------------------


def build_midsection(col_dev, mats, mid_housing_top_z):
    guide_y = DEV_Y_MID_FRONT - 0.015
    add_torus(
        "TOW_FairleadGuide", major_radius=0.016, minor_radius=0.004,
        location=(0, guide_y, mid_housing_top_z + 0.02),
        direction=Vector((0, 1, 0)), major_segments=20, minor_segments=8,
        collection=col_dev, material=mats["MAT_Bare_Steel"],
    )

    # defeated interlock (ruling 4): a base bracket that mounts flush, and
    # a tab that no longer connects to it -- visible gap, not a boolean
    # notch, but unambiguous at prototype scale.
    lx = 0.02
    ly = DEV_Y_REAR_MID + 0.02
    lz = mid_housing_top_z
    add_box(
        "TOW_InterlockBase", size=(0.02, 0.014, 0.008),
        location=(lx, ly, lz + 0.004), collection=col_dev,
        material=mats["MAT_NOX_Addon"],
    )
    add_box(
        "TOW_InterlockTab_Disconnected", size=(0.012, 0.006, 0.005),
        location=(lx, ly + 0.016, lz + 0.006), collection=col_dev,
        material=mats["MAT_Rust_Accent"],
    )

    # scratched-out rating plate (ruling 4)
    px = -0.021
    py = DEV_Y_MID_FRONT - 0.02
    pz = mid_housing_top_z
    add_box(
        "TOW_RatingPlate", size=(0.028, 0.018, 0.002),
        location=(px, py, pz + 0.001), collection=col_dev,
        material=mats["MAT_NOX_Addon"],
    )
    for i in range(3):
        sy = py - 0.005 + i * 0.005
        add_box(
            f"TOW_RatingScratch_{i:02d}", size=(0.024, 0.0015, 0.0008),
            location=(px, sy, pz + 0.0022), collection=col_dev,
            material=mats["MAT_Rust_Accent"],
        )


def build_barrel_and_hook(col_dev, mats, front_housing_top_z):
    barrel_z = front_housing_top_z + BARREL_RADIUS + 0.008
    barrel_y0 = DEV_Y_END - 0.01
    barrel_y_c = barrel_y0 + BARREL_LEN / 2.0

    add_cylinder(
        "TOW_BarrelCollar", radius=BARREL_RADIUS + 0.006, depth=0.02,
        location=(0, barrel_y0 + 0.01, barrel_z), direction=Vector((0, 1, 0)),
        vertices=16, collection=col_dev, material=mats["MAT_FICSIT_Base"],
    )
    add_cylinder(
        "TOW_Barrel", radius=BARREL_RADIUS, depth=BARREL_LEN,
        location=(0, barrel_y_c, barrel_z), direction=Vector((0, 1, 0)),
        vertices=16, collection=col_dev, material=mats["MAT_Bare_Steel"],
    )

    tip = Vector((0, barrel_y0 + BARREL_LEN, barrel_z))
    y_axis = Vector((0, 1, 0))
    for i in range(BARB_COUNT):
        ang = (2 * math.pi / BARB_COUNT) * i
        radial = Vector((math.cos(ang), 0, math.sin(ang)))
        start = tip + radial * BARREL_RADIUS

        dir_a = (y_axis * 0.75 + radial * 0.55).normalized()
        len_a = 0.035
        rad_a = 0.006
        center_a = start + dir_a * (len_a / 2.0)
        add_cylinder(
            f"TOW_Barb{i}_A", radius=rad_a, depth=len_a, location=center_a,
            direction=dir_a, vertices=10, collection=col_dev,
            material=mats["MAT_Bare_Steel"],
        )

        from mathutils import Matrix
        bend_axis = radial.cross(y_axis)
        if bend_axis.length < 1e-6:
            bend_axis = Vector((0, 0, 1))
        bend_axis.normalize()
        rot = Matrix.Rotation(math.radians(115), 4, bend_axis)
        dir_b = (rot @ dir_a).normalized()
        end_a = start + dir_a * len_a
        start_b = end_a - dir_a * 0.004  # slight overlap, no visible gap
        len_b = 0.022
        rad_b = 0.0045
        center_b = start_b + dir_b * (len_b / 2.0)
        add_cylinder(
            f"TOW_Barb{i}_B", radius=rad_b, depth=len_b, location=center_b,
            direction=dir_b, vertices=10, collection=col_dev,
            material=mats["MAT_Bare_Steel"],
        )


# ---------------------------------------------------------------------------
# build: cuffs + standoff struts
# ---------------------------------------------------------------------------


def build_cuffs(col_dev, mats, housing_bottom_lookup):
    for name, y in (("Rear", CUFF_REAR_Y), ("Front", CUFF_FRONT_Y)):
        r = arm_radius(y)
        major = r + CUFF_GAP
        cuff = add_torus(
            f"TOW_Cuff{name}", major_radius=major,
            minor_radius=CUFF_THICKNESS / 2.0, location=(0, y, 0),
            direction=Vector((0, 1, 0)), major_segments=28, minor_segments=8,
            collection=col_dev, material=mats["MAT_FICSIT_Base"],
        )
        housing_bottom_z = housing_bottom_lookup(y)
        for sx in (-1, 1):
            top_of_cuff = Vector((0, y, major))
            strut_top = Vector((sx * 0.014, y, housing_bottom_z))
            strut_vec = strut_top - top_of_cuff
            strut_len = strut_vec.length
            if strut_len < 1e-4:
                continue
            strut_center = top_of_cuff + strut_vec * 0.5
            add_cylinder(
                f"TOW_Strut_{name}{'L' if sx < 0 else 'R'}", radius=0.0045,
                depth=strut_len, location=strut_center,
                direction=strut_vec, vertices=8, collection=col_dev,
                material=mats["MAT_FICSIT_Base"],
            )


# ---------------------------------------------------------------------------
# camera / lighting / render
# ---------------------------------------------------------------------------


def collection_bounds(collections):
    mins = Vector((1e9, 1e9, 1e9))
    maxs = Vector((-1e9, -1e9, -1e9))
    found = False
    for col in collections:
        for obj in col.objects:
            if obj.type != 'MESH':
                continue
            found = True
            for corner in obj.bound_box:
                world_corner = obj.matrix_world @ Vector(corner)
                mins.x = min(mins.x, world_corner.x)
                mins.y = min(mins.y, world_corner.y)
                mins.z = min(mins.z, world_corner.z)
                maxs.x = max(maxs.x, world_corner.x)
                maxs.y = max(maxs.y, world_corner.y)
                maxs.z = max(maxs.z, world_corner.z)
    if not found:
        return Vector((0, 0, 0)), 0.1
    center = (mins + maxs) / 2.0
    radius = (maxs - mins).length / 2.0
    return center, max(radius, 0.02)


def point_camera_at(cam_obj, target):
    direction = target - cam_obj.location
    cam_obj.rotation_mode = 'QUATERNION'
    cam_obj.rotation_quaternion = direction.to_track_quat('-Z', 'Y')


def setup_camera_and_lights():
    cam_data = bpy.data.cameras.new("TOW_Camera")
    cam_data.lens = 50
    cam_obj = bpy.data.objects.new("TOW_Camera", cam_data)
    bpy.context.scene.collection.objects.link(cam_obj)
    bpy.context.scene.camera = cam_obj

    key = bpy.data.lights.new("TOW_KeyLight", type='SUN')
    key.energy = 3.2
    key_obj = bpy.data.objects.new("TOW_KeyLight", key)
    bpy.context.scene.collection.objects.link(key_obj)
    key_obj.rotation_euler = (math.radians(55), 0, math.radians(35))

    fill = bpy.data.lights.new("TOW_FillLight", type='AREA')
    fill.energy = 90
    fill.size = 0.6
    fill_obj = bpy.data.objects.new("TOW_FillLight", fill)
    bpy.context.scene.collection.objects.link(fill_obj)
    fill_obj.location = (-0.4, -0.2, 0.3)
    point_camera_at(fill_obj, Vector((0, 0.13, 0.05)))

    world = bpy.data.worlds.get("World") or bpy.data.worlds.new("World")
    bpy.context.scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg is not None:
        bg.inputs[0].default_value = (0.82, 0.82, 0.82, 1.0)
        bg.inputs[1].default_value = 0.9

    return cam_obj


def frame_camera(cam_obj, target_collections, azimuth_deg, elevation_deg,
                  extra_pad=1.6):
    center, radius = collection_bounds(target_collections)
    dist = radius * extra_pad / math.tan(math.radians(cam_obj.data.angle / 2.0))
    az = math.radians(azimuth_deg)
    el = math.radians(elevation_deg)
    offset = Vector((
        math.cos(el) * math.sin(az),
        -math.cos(el) * math.cos(az),
        math.sin(el),
    )) * dist
    cam_obj.location = center + offset
    point_camera_at(cam_obj, center)


def set_render_engine():
    scene = bpy.context.scene
    for engine in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE', 'CYCLES'):
        try:
            scene.render.engine = engine
            print(f"[TOW] render engine set to {engine}")
            return
        except TypeError:
            continue
    print("[TOW] WARNING: no expected render engine accepted, using default"
          f" {scene.render.engine}")


def render_view(name, cam_obj, col_dev, col_ref, azimuth_deg, elevation_deg,
                 show_reference):
    col_ref.hide_render = not show_reference
    frame_camera(
        cam_obj,
        [col_dev, col_ref] if show_reference else [col_dev],
        azimuth_deg, elevation_deg,
    )
    scene = bpy.context.scene
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1200
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGB'
    out_path = os.path.join(RENDER_DIR, f"{name}.png")
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)
    assert os.path.exists(out_path), f"render did not produce {out_path}"
    size = os.path.getsize(out_path)
    assert size > 0, f"render {out_path} is zero bytes"
    print(f"[TOW] rendered {name}: {out_path} ({size} bytes)")
    return out_path, size


# ---------------------------------------------------------------------------
# export + validation
# ---------------------------------------------------------------------------


def export_glb(col_dev):
    bpy.ops.object.select_all(action='DESELECT')
    active_set = False
    for obj in col_dev.objects:
        obj.select_set(True)
        if not active_set:
            bpy.context.view_layer.objects.active = obj
            active_set = True
    bpy.ops.export_scene.gltf(
        filepath=GLB_PATH,
        export_format='GLB',
        use_selection=True,
        export_yup=True,
        export_apply=True,
    )
    assert os.path.exists(GLB_PATH), f"glTF export did not produce {GLB_PATH}"
    size = os.path.getsize(GLB_PATH)
    assert size > 0, "exported glb is zero bytes"
    with open(GLB_PATH, 'rb') as f:
        header = f.read(12)
        magic, version, length = struct.unpack('<4sII', header)
        assert magic == b'glTF', f"exported file has bad glTF magic: {magic}"
    print(f"[TOW] glb validated: magic={magic} version={version} "
          f"declared_length={length} bytes_on_disk={size}")
    return size


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main():
    os.makedirs(RENDER_DIR, exist_ok=True)

    clear_scene()
    setup_units()
    mats = make_materials()
    col_dev, col_ref = make_collections()

    build_forearm_proxy(col_ref, mats)
    housings, bolts = build_housing(col_dev, mats)

    rear_top_z = arm_radius((DEV_Y_START + DEV_Y_REAR_MID) / 2.0) + \
        STANDOFF + HOUSING_THK
    mid_top_z = arm_radius((DEV_Y_REAR_MID + DEV_Y_MID_FRONT) / 2.0) + \
        STANDOFF + HOUSING_THK
    front_top_z = arm_radius((DEV_Y_MID_FRONT + DEV_Y_END) / 2.0) + \
        STANDOFF + HOUSING_THK

    build_reel(col_dev, mats, rear_top_z)
    build_midsection(col_dev, mats, mid_top_z)
    build_barrel_and_hook(col_dev, mats, front_top_z)

    def housing_bottom_at(y):
        return arm_radius(y) + STANDOFF

    build_cuffs(col_dev, mats, housing_bottom_at)

    cam_obj = setup_camera_and_lights()
    set_render_engine()

    print(f"[TOW] device object count: {len(col_dev.objects)}")
    print(f"[TOW] reference object count: {len(col_ref.objects)}")

    bpy.ops.wm.save_as_mainfile(filepath=BLEND_PATH)
    blend_size = os.path.getsize(BLEND_PATH)
    print(f"[TOW] saved blend: {BLEND_PATH} ({blend_size} bytes)")

    glb_size = export_glb(col_dev)

    results = []
    results.append(render_view("side", cam_obj, col_dev, col_ref,
                                azimuth_deg=90, elevation_deg=8,
                                show_reference=False))
    results.append(render_view("top", cam_obj, col_dev, col_ref,
                                azimuth_deg=0, elevation_deg=85,
                                show_reference=False))
    results.append(render_view("three_quarter", cam_obj, col_dev, col_ref,
                                azimuth_deg=35, elevation_deg=28,
                                show_reference=False))
    results.append(render_view("mounted", cam_obj, col_dev, col_ref,
                                azimuth_deg=40, elevation_deg=22,
                                show_reference=True))

    print("[TOW] BUILD COMPLETE")
    print(f"[TOW]   blend: {BLEND_PATH} ({blend_size} bytes)")
    print(f"[TOW]   glb:   {GLB_PATH} ({glb_size} bytes)")
    for path, size in results:
        print(f"[TOW]   render: {path} ({size} bytes)")


if __name__ == "__main__":
    main()
