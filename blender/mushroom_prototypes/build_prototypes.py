"""Run with Blender --background --factory-startup --python <this file>."""
import bpy
import json
import math
import random
import sys
from pathlib import Path
from mathutils import Vector, Matrix

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[1]
CLUSTER = '--cluster' in sys.argv
if CLUSTER:
    OUT = OUT / 'cluster'
    OUT.mkdir(parents=True, exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
assets = bpy.data.collections.new('Mushroom prototypes')
bpy.context.scene.collection.children.link(assets)
stage = bpy.data.collections.new('Preview studio (not part of models)')
bpy.context.scene.collection.children.link(stage)

def move_collection(obj, collection):
    for c in list(obj.users_collection):
        c.objects.unlink(obj)
    collection.objects.link(obj)

def atlas(name, glow=False):
    source = bpy.data.images.load(str(ROOT / 'assets/blocks/textures' / (name + '.png')))
    pixels = list(source.pixels)
    palette = {tuple(round(v * 255) for v in pixels[i:i+3]) for i in range(0, len(pixels), 4) if pixels[i+3] > .5}
    if glow:
        top = [(242,178,73), (242,181,76), (246,193,98), (247,196,104)]
        side = [(229,137,0), (197,119,9), (237,163,45)]
        stem = [(185,146,91), (174,136,79), (171,133,76), (162,124,66)]
        under = [(175,123,53), (148,101,41), (154,108,47)]
    else:
        top = [(205,154,121), (191,144,113), (170,128,99), (178,134,104)]
        side = [(146,109,84), (134,100,77), (115,85,65), (164,124,96)]
        stem = [(104,91,81), (94,83,75), (88,78,69), (71,62,55)]
        under = [(115,85,65), (134,100,77), (88,78,69)]
    assert all(c in palette for colors in (top,side,stem,under) for c in colors)
    # Compress side-color variation around its original linear-light mean,
    # retaining the palette's overall brightness and hue.
    def linear(v):
        v /= 255
        return v/12.92 if v <= .04045 else ((v+.055)/1.055)**2.4
    def srgb(v):
        return 255*(12.92*v if v <= .0031308 else 1.055*v**(1/2.4)-.055)
    side_linear = [[linear(v) for v in color] for color in side]
    mean = [sum(c[i] for c in side_linear)/len(side_linear) for i in range(3)]
    side = [tuple(round(srgb(mean[i]+.45*(c[i]-mean[i]))) for i in range(3)) for c in side_linear]
    rng = random.Random(12 if glow else 7)
    data = []
    for y in range(16):
        for x in range(16):
            colors = top if x < 8 and y < 8 else side if x < 8 and y < 10 else under if x < 8 else stem
            if colors is top:
                # Restrained single-texel mottling; no cream zigzag in the yellow cap.
                k = rng.choices(range(len(colors)), weights=[6,4,2,1])[0]
            elif colors is stem:
                k = (x + y + rng.randrange(2)) % len(colors)
            else:
                k = (x + y) % len(colors)
            data.extend([v/255 for v in colors[k]] + [1])
    if glow and CLUSTER:
        # Three disjoint top islands in the existing 8x8 cap region. Give each
        # head a reproducible shuffle, with restrained highlights in the same palette.
        for ox, oy, size, seed in ((0,0,5,23),(5,0,3,47),(5,3,3,89)):
            # Nine texels are too few for isolated bright accents: they read as
            # eyes/spots. Use evenly represented, neighboring gold shades instead.
            cap_colors = top if size==5 else [(238,166,51),(242,178,73),(242,181,76)]
            assert all(c in palette for c in cap_colors)
            shades = ([0]*13+[1]*7+[2]*4+[3]) if size==5 else ([0,1,2]*3)
            random.Random(seed).shuffle(shades)
            for y in range(size):
                for x in range(size):
                    idx=((oy+y)*16+ox+x)*4
                    data[idx:idx+4]=[v/255 for v in cap_colors[shades[y*size+x]]]+[1]
        side_rng = random.Random(136)
        for y in (8,9):
            for x in range(8):
                idx=(y*16+x)*4
                data[idx:idx+4]=[v/255 for v in side[side_rng.randrange(len(side))]]+[1]
    image = bpy.data.images.new(name + '_atlas_16', width=16, height=16, alpha=True)
    image.pixels = data
    image.filepath_raw = str(OUT / (name + '_16.png'))
    image.file_format = 'PNG'
    image.save()
    image.pack()
    mat = bpy.data.materials.new(name + ' | 16x16 nearest')
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Roughness'].default_value = .88
    tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
    tex.image = image
    tex.interpolation = 'Closest'
    mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    return mat

brown_mat = atlas('brown_mushroom')
glow_mat = atlas('glowshroom_yellow', True)

def root_object(name, x):
    obj = bpy.data.objects.new(name, None)
    assets.objects.link(obj)
    obj.location.x = x
    obj['units'] = 'One Blender unit = one voxel; dimensions authored on a 1/16 grid.'
    return obj

brown = root_object('Brown mushroom — 36 triangles', -.44)
glow = root_object('Yellow glowshroom cluster — 108 triangles' if CLUSTER else 'Yellow glowshroom pair — 72 triangles', .42)

def box(name, center, size, parent, material, stem=False, tilt=0, omit=(), cap_variant=0):
    sx,sy,sz = (s/2 for s in size)
    corners = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
               (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    rotation = Matrix.Rotation(math.radians(tilt), 3, 'Y')
    vertices = [(rotation @ Vector(v) + Vector(center))/16 for v in corners]
    faces = [(0,3,2,1),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7),(4,5,6,7)]
    mesh = bpy.data.meshes.new(name)
    face_ids = [i for i in range(6) if i not in omit]
    mesh.from_pydata(vertices, [], [faces[i] for i in face_ids])
    mesh.update()
    uv = mesh.uv_layers.new(name='16x16 pixel atlas')
    for face in mesh.polygons:
        if stem:
            origin, capacity = (8, 0), (8, 16)
        elif face_ids[face.index] == 5:
            origin, capacity = (0, 0), (8, 8)
        elif face_ids[face.index] == 0:
            origin, capacity = (0, 10), (8, 6)
        else:
            origin, capacity = (0, 8), (8, 2)
        if CLUSTER and material==glow_mat and not stem:
            if face_ids[face.index]==5:
                origin, capacity = (((0,0),(5,0),(5,3))[cap_variant], (5,5) if cap_variant==0 else (3,3))
            elif face_ids[face.index] in (1,2,3,4):
                origin, capacity = (((0,8),(5,8),(5,9))[cap_variant], (5,1) if cap_variant==0 else (3,1))
        # Unfold each planar face in an orthonormal basis. One world unit in
        # either direction maps to one UV unit (16 texels per voxel). Unlike
        # rectangular UV fitting this also preserves squares on sheared faces.
        points = [mesh.vertices[i].co for i in face.vertices]
        tangent = (points[1] - points[0]).normalized()
        bitangent = face.normal.cross(tangent).normalized()
        coords = [Vector(((p-points[0]).dot(tangent)*16,
                          (p-points[0]).dot(bitangent)*16)) for p in points]
        minimum = Vector((min(p.x for p in coords), min(p.y for p in coords)))
        coords = [p-minimum for p in coords]
        if max(p.x for p in coords) > capacity[0]+1e-5 or max(p.y for p in coords) > capacity[1]+1e-5:
            coords = [Vector((p.y,p.x)) for p in coords]
        assert max(p.x for p in coords) <= capacity[0]+1e-5
        assert max(p.y for p in coords) <= capacity[1]+1e-5
        for idx, co in zip(face.loop_indices, coords):
            uv.data[idx].uv = ((origin[0]+co.x)/16,(origin[1]+co.y)/16)
    obj = bpy.data.objects.new(name, mesh)
    assets.objects.link(obj)
    obj.parent = parent
    obj.data.materials.append(material)
    return obj

box('Brown | stem', (0,0,1.5), (2,2,3), brown,brown_mat,True)
box('Brown | cap rim', (0,0,4), (6,6,2), brown,brown_mat)
box('Brown | cap crown', (0,0,5.5), (4,4,1), brown,brown_mat)

def elbow(label, pivot, outer, turned):
    # Triangular prism filling the turn between two true rectangular stems.
    points = [Vector((p.x,p.y+y,p.z))/16 for y in (-.5,.5) for p in (pivot,outer,turned)]
    faces = [(0,1,2),(3,5,4),(1,4,5,2)]
    center = sum(points,Vector())/6
    oriented = []
    for face in faces:
        a,b,c = (points[i] for i in face[:3])
        middle = sum((points[i] for i in face),Vector())/len(face)
        oriented.append(tuple(reversed(face)) if (b-a).cross(c-a).dot(middle-center)<0 else face)
    mesh = bpy.data.meshes.new(label+' | triangular bend')
    mesh.from_pydata(points,[],oriented)
    mesh.update()
    uv = mesh.uv_layers.new(name='Single pixel bend')
    for face in mesh.polygons:
        ps = [mesh.vertices[i].co for i in face.vertices]
        u = (ps[1]-ps[0]).normalized()
        v = face.normal.cross(u).normalized()
        coords = [Vector(((p-ps[0]).dot(u)*16,(p-ps[0]).dot(v)*16)) for p in ps]
        minimum = Vector((min(p.x for p in coords),min(p.y for p in coords)))
        coords = [p-minimum for p in coords]
        assert max(p.x for p in coords)<=1.00001 and max(p.y for p in coords)<=1.00001
        for idx,co in zip(face.loop_indices,coords):
            uv.data[idx].uv=((9+co.x)/16,(1+co.y)/16)
    obj=bpy.data.objects.new(label+' | triangular bend',mesh)
    assets.objects.link(obj)
    obj.parent=glow
    obj.data.materials.append(glow_mat)


def shroom(label, base, lower, upper, angle, capsize, yaw=0, cap_variant=0):
    previous_parts = set(glow.children)
    assert int(lower)==lower and int(upper)==upper
    rot=Matrix.Rotation(math.radians(angle),3,'Y')
    direction=rot @ Vector((0,0,1))
    across=rot @ Vector((1,0,0))
    sign=1 if angle>0 else -1
    pivot=Vector((base[0]+sign*.5,base[1],lower))
    outer=pivot-Vector((sign,0,0))
    turned=pivot-across*sign
    upper_base=(pivot+turned)/2
    tip=upper_base+direction*upper
    box(label+' | lower stem',(base[0],base[1],lower/2),(1,1,lower),glow,glow_mat,True,omit=(5,))
    box(label+' | leaning stem',upper_base+direction*upper/2,(1,1,upper),glow,glow_mat,True,angle,omit=(0,))
    elbow(label,pivot,outer,turned)
    box(label+' | cap',tip+direction*.5,(*capsize,1),glow,glow_mat,False,angle,cap_variant=cap_variant)
    spin = Matrix.Rotation(math.radians(yaw),3,'Z')
    anchor = Vector((base[0],base[1],0))/16
    for obj in set(glow.children)-previous_parts:
        for vertex in obj.data.vertices:
            vertex.co = anchor + spin @ (vertex.co-anchor)
        obj.data.update()

if CLUSTER:
    shroom('Tall glowshroom',(0,.8),3,3,20,(5,5),90)
    shroom('Small left glowshroom',(-1.4,-.8),1,2,26,(3,3),210,1)
    shroom('Small right glowshroom',(1.4,-1),1,1,18,(3,3),-30,2)
    for obj in brown.children:
        obj.hide_render = True
else:
    shroom('Tall glowshroom',(-.8,.5),3,3,-18,(8,5))
    shroom('Small glowshroom',(1.1,-.7),1,2,24,(4,3))

report = {}
for root in (brown,glow):
    children = list(root.children)
    tris = sum(sum(len(p.vertices)-2 for p in o.data.polygons) for o in children)
    assert all(-1e-6 <= c <= 1+1e-6 for o in children for uv in o.data.uv_layers.active.data for c in uv.uv)
    max_error = 0
    for obj in children:
        for face in obj.data.polygons:
            loops = list(face.loop_indices)
            # Validate square texel scale on every face, including diagonals.
            for i in range(len(loops)):
                for j in range(i):
                    a,b = loops[i],loops[j]
                    world_distance = (obj.data.vertices[obj.data.loops[a].vertex_index].co-
                                      obj.data.vertices[obj.data.loops[b].vertex_index].co).length
                    uv_distance = (obj.data.uv_layers.active.data[a].uv-
                                   obj.data.uv_layers.active.data[b].uv).length
                    max_error = max(max_error, abs(uv_distance/world_distance-1))
    assert max_error < 1e-5, (root.name, max_error)
    report[root.name] = {'triangles':tris,'mesh_parts':len(children),'texture_size':[16,16],
                        'uv_bounds':[0,1], 'texels_per_block':16,
                        'max_relative_checked_distance_error':max_error,
                        'stem_segments_pixels': ({'tall':[3,3], 'small_left':[1,2], 'small_right':[1,1]} if CLUSTER else {'tall':[3,3], 'small':[1,2]}) if root==glow else [3],
                        'tilted_stem_mapping':'rigidly rotated square pixels; single-pixel triangular elbow' if root==glow else 'axis aligned'}
(OUT/'validation.json').write_text(json.dumps(report,indent=2))

def plain(name, color):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*color,1)
    return mat

bpy.ops.mesh.primitive_plane_add(size=200)
floor = bpy.context.object
floor.name = 'Studio ground'
floor.location.z = 0
floor.data.materials.append(plain('Studio slate',(.075,.092,.10)))
move_collection(floor,stage)

scene = bpy.context.scene
scene.render.engine = 'CYCLES'
cycles_preferences = bpy.context.preferences.addons['cycles'].preferences
cycles_preferences.compute_device_type = 'OPTIX'
cycles_preferences.refresh_devices()
for device in cycles_preferences.devices:
    device.use = device.type == 'OPTIX'
assert any(d.use for d in cycles_preferences.devices), 'OptiX GPU required; do not fall back to CPU'
scene.cycles.device = 'GPU'
print('RENDER_BACKEND: OPTIX; CPU disabled;', [d.name for d in cycles_preferences.devices if d.use], flush=True)
scene.cycles.samples = 48
scene.cycles.use_denoising = True
scene.cycles.denoiser = 'OPTIX'
scene.render.resolution_x = 1400
scene.render.resolution_y = 850
scene.render.resolution_percentage = 100
scene.world.color = (.3,.3,.3)
scene.view_settings.view_transform = 'Standard'

def aim(obj,target):
    obj.rotation_euler = (Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()

for name,loc,power,size in [('Key',(-2,-3,5),400,4),('Fill',(3,-1,3),180,3)]:
    data=bpy.data.lights.new(name,'AREA')
    data.energy=power
    data.shape='DISK'
    data.size=size
    obj=bpy.data.objects.new(name,data)
    stage.objects.link(obj)
    obj.location=loc
    aim(obj,(0,0,.2))

camera_data=bpy.data.cameras.new('Preview camera')
camera=bpy.data.objects.new('Preview camera',camera_data)
stage.objects.link(camera)
scene.camera=camera
camera_data.type='ORTHO'
camera_data.ortho_scale=1.65
target = Vector((0,0,.22))
if CLUSTER:
    target = Vector((glow.location.x,0,.24))
    camera_data.ortho_scale = 1.12

for name,loc in [('preview', (1.1,-2.8,1.6)),('front',(0,-3,.5)),('rear',(-1.2,2.8,1.3))]:
    camera.location=Vector(loc)+(Vector((glow.location.x,0,0)) if CLUSTER else Vector())
    aim(camera,target)
    scene.render.filepath=str(OUT/(name+'.png'))
    bpy.ops.render.render(write_still=True)

camera.location=Vector((1.1,-2.8,1.6))+(Vector((glow.location.x,0,0)) if CLUSTER else Vector())
aim(camera,target)
bpy.ops.object.select_all(action='DESELECT')
for obj in (glow if CLUSTER else brown).children:
    obj.select_set(True)
bpy.context.view_layer.objects.active=(glow if CLUSTER else brown).children[0]
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type=='VIEW_3D':
            area.spaces.active.region_3d.view_perspective='CAMERA'
            area.spaces.active.shading.color_type='TEXTURE'
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'mushroom_prototypes.blend'))
print('PROTOTYPE_VALIDATION',json.dumps(report))
