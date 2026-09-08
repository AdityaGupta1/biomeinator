"""Export the approved prototype blend as terrain GLBs and 16px textures.

blender -b --factory-startup --python blender/mushroom_prototypes/export_block_models.py
No rendering is needed. The preview studio and presentation offsets are excluded.
"""
import bpy
import shutil
from pathlib import Path
from mathutils import Matrix

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MODELS = ROOT / 'assets/blocks/models'
TEXTURES = ROOT / 'assets/blocks/textures'
MODELS.mkdir(parents=True, exist_ok=True)

for prefix, name in [('Brown mushroom', 'brown_mushroom'), ('Yellow glowshroom', 'glowshroom_yellow')]:
    source = HERE / 'cluster' if name == 'glowshroom_yellow' else HERE
    bpy.ops.wm.open_mainfile(filepath=str(source/'mushroom_prototypes.blend'))
    parent = next(o for o in bpy.data.objects if o.type == 'EMPTY' and o.name.startswith(prefix))
    bpy.ops.object.select_all(action='DESELECT')
    exported = []
    for child in parent.children:
        obj = bpy.data.objects.new(child.name, child.data.copy())
        bpy.context.scene.collection.objects.link(obj)
        obj.matrix_world = Matrix.Identity(4)
        obj.select_set(True)
        exported.append(obj)
    bpy.context.view_layer.objects.active = exported[0]
    assert min(v.co.z for o in exported for v in o.data.vertices) == 0
    bpy.ops.export_scene.gltf(filepath=str(MODELS/(name+'.glb')), export_format='GLB',
        use_selection=True, export_yup=True, export_texcoords=True, export_normals=True,
        export_materials='NONE', export_cameras=False, export_lights=False, export_animations=False)
    for obj in exported:
        bpy.data.objects.remove(obj, do_unlink=True)
    # Read the on-disk atlas, including manual edits exported from GIMP.
    shutil.copyfile(source/(name+'_16.png'), TEXTURES/(name+'_model.png'))

# The atlas reserves its left half for cap top/side/underside, right half for stems.
# R is linear emissive strength; G (biome tint) and B are zero. Fully opaque alpha.
aux = bpy.data.images.new('Glowshroom cap emission', width=16, height=16, alpha=True)
aux.colorspace_settings.name = 'Non-Color'
aux.pixels = [value for y in range(16) for x in range(16) for value in (1.0 if x < 8 else 0.0, 0.0, 0.0, 1.0)]
aux.filepath_raw = str(TEXTURES/'glowshroom_yellow_model.aux.png')
aux.file_format = 'PNG'
aux.save()
print('Exported both block models and opaque atlases; glowshroom caps have emission, stems do not.')
