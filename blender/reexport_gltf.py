"""Headless driver: convert a .blend's materials to the Biomeinator BSDF group and export glTF.

Usage:
    blender --background <file.blend> --python reexport_gltf.py -- --gltf <out.gltf> [--convert] [--save-blend]

--convert rewires Principled BSDF / Emission materials to the Biomeinator BSDF node
group (preserving texture links); --save-blend saves the .blend afterwards.
"""

import argparse
import importlib.util
import os
import sys

import bpy

_addon_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'biomeinator_bsdf.py')
_spec = importlib.util.spec_from_file_location('biomeinator_bsdf', _addon_path)
biomeinator_bsdf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(biomeinator_bsdf)


def _copy_to_group(node_tree, surface_node, socket_name, group_node, group_socket_name):
    src = surface_node.inputs[socket_name]
    dst = group_node.inputs[group_socket_name]
    if src.is_linked:
        node_tree.links.new(src.links[0].from_socket, dst)
    else:
        dst.default_value = src.default_value


def convert_material(material, ng):
    """Rewires a Principled BSDF or Emission material to the Biomeinator BSDF group.

    Returns True if converted, False if skipped (already converted or unrecognized).
    """
    if not material.use_nodes:
        return False
    if biomeinator_bsdf.find_group_node(material) is not None:
        return False

    node_tree = material.node_tree
    output = next((n for n in node_tree.nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output), None)
    if output is None or not output.inputs['Surface'].is_linked:
        print(f'skipping {material.name}: no surface node')
        return False
    surface_node = output.inputs['Surface'].links[0].from_node

    group = node_tree.nodes.new('ShaderNodeGroup')
    group.node_tree = ng
    group.location = surface_node.location

    if surface_node.type == 'BSDF_PRINCIPLED':
        metallic = surface_node.inputs['Metallic'].default_value
        specularLevel = surface_node.inputs['Specular IOR Level'].default_value
        transmission = surface_node.inputs['Transmission Weight'].default_value

        isMetal = metallic >= 1.0
        hasTransmission = transmission > 0.0
        hasSpecular = isMetal or specularLevel > 0.0
        hasDiffuse = not isMetal and not hasTransmission

        group.inputs['Diffuse'].default_value = hasDiffuse
        group.inputs['Specular'].default_value = hasSpecular
        group.inputs['Transmission'].default_value = hasTransmission
        if hasSpecular:
            group.inputs['Roughness'].default_value = surface_node.inputs['Roughness'].default_value
        _copy_to_group(node_tree, surface_node, 'Base Color', group, 'Base Color')
        if isMetal:
            _copy_to_group(node_tree, surface_node, 'Base Color', group, 'Specular Tint')
        elif hasSpecular:
            _copy_to_group(node_tree, surface_node, 'Specular Tint', group, 'Specular Tint')
        _copy_to_group(node_tree, surface_node, 'IOR', group, 'IOR')
        _copy_to_group(node_tree, surface_node, 'Emission Color', group, 'Emission Color')
        _copy_to_group(node_tree, surface_node, 'Emission Strength', group, 'Emission Strength')
        _copy_to_group(node_tree, surface_node, 'Alpha', group, 'Alpha')
    elif surface_node.type == 'EMISSION':
        group.inputs['Base Color'].default_value = (0.0, 0.0, 0.0, 1.0)
        _copy_to_group(node_tree, surface_node, 'Color', group, 'Emission Color')
        _copy_to_group(node_tree, surface_node, 'Strength', group, 'Emission Strength')
    elif surface_node.type == 'BSDF_GLOSSY':
        group.inputs['Diffuse'].default_value = False
        group.inputs['Specular'].default_value = True
        group.inputs['Roughness'].default_value = surface_node.inputs['Roughness'].default_value
        _copy_to_group(node_tree, surface_node, 'Color', group, 'Specular Tint')
    elif surface_node.type == 'MIX_SHADER':
        def linked_node(socket):
            return socket.links[0].from_node if socket.is_linked else None

        fac_node = linked_node(surface_node.inputs[0])
        base_node = linked_node(surface_node.inputs[1])
        gloss_node = linked_node(surface_node.inputs[2])
        isFresnelMix = (fac_node is not None and fac_node.type == 'FRESNEL'
                        and base_node is not None and gloss_node is not None
                        and gloss_node.type == 'BSDF_GLOSSY'
                        and base_node.type in ('BSDF_REFRACTION', 'BSDF_DIFFUSE'))
        if not isFresnelMix:
            print(f'skipping {material.name}: unrecognized mix shader setup')
            node_tree.nodes.remove(group)
            return False

        hasTransmission = base_node.type == 'BSDF_REFRACTION'
        group.inputs['Diffuse'].default_value = not hasTransmission
        group.inputs['Specular'].default_value = True
        group.inputs['Transmission'].default_value = hasTransmission
        _copy_to_group(node_tree, base_node, 'Color', group, 'Base Color')
        _copy_to_group(node_tree, gloss_node, 'Color', group, 'Specular Tint')
        if hasTransmission:
            _copy_to_group(node_tree, base_node, 'IOR', group, 'IOR')
        else:
            _copy_to_group(node_tree, fac_node, 'IOR', group, 'IOR')
        node_tree.nodes.remove(fac_node)
        node_tree.nodes.remove(base_node)
        node_tree.nodes.remove(gloss_node)
    else:
        print(f'skipping {material.name}: unrecognized surface node {surface_node.type}')
        node_tree.nodes.remove(group)
        return False

    node_tree.nodes.remove(surface_node)
    node_tree.links.new(group.outputs['BSDF'], output.inputs['Surface'])
    print(f'converted {material.name}')
    return True


def main():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument('--gltf', required=True)
    parser.add_argument('--convert', action='store_true')
    parser.add_argument('--save-blend', action='store_true')
    args = parser.parse_args(argv)

    ng = biomeinator_bsdf.ensure_node_group()

    if args.convert:
        for material in bpy.data.materials:
            convert_material(material, ng)

        usedMaterials = set()
        for obj in bpy.data.objects:
            if obj.type == 'MESH':
                usedMaterials.update(m for m in obj.data.materials if m is not None)
        for material in list(bpy.data.materials):
            if material not in usedMaterials and not material.is_grease_pencil:
                print(f'removing unused material {material.name}')
                bpy.data.materials.remove(material)

        # Superseded "_export" duplicates are removed above; drop the now-redundant suffix
        for material in bpy.data.materials:
            if material.name.endswith('_render') and bpy.data.materials.get(material.name[:-len('_render')]) is None:
                material.name = material.name[:-len('_render')]

    if args.save_blend:
        bpy.ops.wm.save_mainfile()

    biomeinator_bsdf.export_gltf(os.path.abspath(args.gltf))


main()
