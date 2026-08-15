import bpy
from mathutils import Vector

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)

RAD, LEN = 0.026, 0.062

def mk(name, direction, up):
    bpy.ops.mesh.primitive_cone_add(vertices=20, radius1=RAD, radius2=RAD,
                                    depth=LEN, location=(0, 0, 0))
    o = bpy.context.object
    o.name = name
    d = direction.normalized()
    o.rotation_mode = 'QUATERNION'
    o.rotation_quaternion = d.to_track_quat('Z', up)
    return o

a = mk("dir_X_up_X", Vector((1, 0, 0)), 'X')
b = mk("dir_X_up_Y", Vector((1, 0, 0)), 'Y')
bpy.context.view_layer.update()

for o in (a, b):
    dims = o.dimensions
    # true world-space extent from the evaluated bounding box corners
    ws = [o.matrix_world @ Vector(c) for c in o.bound_box]
    wx = max(v.x for v in ws) - min(v.x for v in ws)
    wy = max(v.y for v in ws) - min(v.y for v in ws)
    wz = max(v.z for v in ws) - min(v.z for v in ws)
    print(f"{o.name:12} quat={tuple(round(q,3) for q in o.rotation_quaternion)}")
    print(f"    obj.dimensions  x={dims.x:.4f} y={dims.y:.4f} z={dims.z:.4f}   <- what the gate reads")
    print(f"    WORLD bbox      x={wx:.4f} y={wy:.4f} z={wz:.4f}   <- the truth")
