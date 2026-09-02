"""Blender addon: the Biomeinator BSDF node group and glTF export.

The node group mirrors the renderer's material model (see
src/rendering/common/common_structs.h): diffuse, specular reflection, and specular
transmission lobes toggled by boolean sockets, with the specular lobe Fresnel-blended
over the base lobe exactly like the renderer's walterFresnel-based lobe selection.

Export goes through the standard glTF exporter: each material using the node group is
temporarily rewired to an equivalent Principled BSDF (chosen so the exporter emits the
fields src/scene/gltf_loader.cpp parses), exported, then restored.
"""

import bpy

bl_info = {
    'name': 'Biomeinator BSDF',
    'author': 'Aditya Gupta',
    'version': (1, 0, 0),
    'blender': (4, 4, 0),
    'location': 'Shader Editor > Add > Group, File > Export > Biomeinator glTF',
    'description': 'Biomeinator material node group and matching glTF export',
    'category': 'Material',
}

NODE_GROUP_NAME = 'Biomeinator BSDF'


def _upgrade_node_group(ng):
    hasRoughness = any(item.item_type == 'SOCKET' and item.in_out == 'INPUT' and item.name == 'Roughness'
                       for item in ng.interface.items_tree)
    if hasRoughness:
        return
    sock = ng.interface.new_socket(name='Roughness', in_out='INPUT', socket_type='NodeSocketFloat')
    sock.default_value = 0.0
    sock.min_value = 0.0
    sock.max_value = 1.0
    sock.subtype = 'FACTOR'
    group_in = next(n for n in ng.nodes if n.type == 'GROUP_INPUT')
    glossy = next(n for n in ng.nodes if n.type == 'BSDF_GLOSSY')
    ng.links.new(group_in.outputs['Roughness'], glossy.inputs['Roughness'])


def ensure_node_group():
    existing = bpy.data.node_groups.get(NODE_GROUP_NAME)
    if existing is not None:
        _upgrade_node_group(existing)
        return existing

    ng = bpy.data.node_groups.new(NODE_GROUP_NAME, 'ShaderNodeTree')

    def add_input(name, socket_type, default=None, min_value=None, max_value=None, subtype=None):
        sock = ng.interface.new_socket(name=name, in_out='INPUT', socket_type=socket_type)
        if default is not None:
            sock.default_value = default
        if min_value is not None:
            sock.min_value = min_value
        if max_value is not None:
            sock.max_value = max_value
        if subtype is not None:
            sock.subtype = subtype
        return sock

    add_input('Base Color', 'NodeSocketColor', (0.8, 0.8, 0.8, 1.0))
    add_input('Diffuse', 'NodeSocketBool', True)
    add_input('Specular', 'NodeSocketBool', False)
    add_input('Specular Tint', 'NodeSocketColor', (1.0, 1.0, 1.0, 1.0))
    # Applies only to specular reflection; rough transmission is not supported by the engine.
    # The Fresnel-node lobe mix uses the macro normal; revisit matching the microfacet normal
    # (Cycles' per-half-vector Fresnel, e.g. via an OSL dielectric_bsdf closure) later.
    add_input('Roughness', 'NodeSocketFloat', 0.0, min_value=0.0, max_value=1.0, subtype='FACTOR')
    add_input('Transmission', 'NodeSocketBool', False)
    add_input('IOR', 'NodeSocketFloat', 1.45, min_value=0.0)
    add_input('Emission Color', 'NodeSocketColor', (1.0, 1.0, 1.0, 1.0))
    add_input('Emission Strength', 'NodeSocketFloat', 0.0, min_value=0.0)
    add_input('Alpha', 'NodeSocketFloat', 1.0, min_value=0.0, max_value=1.0, subtype='FACTOR')

    ng.interface.new_socket(name='BSDF', in_out='OUTPUT', socket_type='NodeSocketShader')

    nodes = ng.nodes
    links = ng.links

    group_in = nodes.new('NodeGroupInput')
    group_in.location = (-800, 0)

    diffuse = nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.location = (-400, 300)

    refraction = nodes.new('ShaderNodeBsdfRefraction')
    refraction.distribution = 'GGX'
    refraction.location = (-400, 100)

    glossy = nodes.new('ShaderNodeBsdfAnisotropic')
    glossy.distribution = 'MULTI_GGX'
    glossy.location = (-400, -100)

    fresnel = nodes.new('ShaderNodeFresnel')
    fresnel.location = (-600, -300)

    has_base = nodes.new('ShaderNodeMath')
    has_base.operation = 'MAXIMUM'
    has_base.label = 'hasBase = max(Diffuse, Transmission)'
    has_base.location = (-600, -450)

    fresnel_or_one = nodes.new('ShaderNodeMix')
    fresnel_or_one.data_type = 'FLOAT'
    fresnel_or_one.label = 'mix(1, Fresnel, hasBase)'
    fresnel_or_one.location = (-400, -350)
    fresnel_or_one.inputs['A'].default_value = 1.0

    spec_fac = nodes.new('ShaderNodeMath')
    spec_fac.operation = 'MULTIPLY'
    spec_fac.label = 'specFac = Specular * that'
    spec_fac.location = (-200, -350)

    mix_base = nodes.new('ShaderNodeMixShader')
    mix_base.label = 'Diffuse vs Transmission'
    mix_base.location = (-150, 250)

    mix_spec = nodes.new('ShaderNodeMixShader')
    mix_spec.label = 'base vs Specular (Fresnel)'
    mix_spec.location = (50, 100)

    emission = nodes.new('ShaderNodeEmission')
    emission.location = (50, -100)

    add_emission = nodes.new('ShaderNodeAddShader')
    add_emission.location = (250, 0)

    transparent = nodes.new('ShaderNodeBsdfTransparent')
    transparent.location = (250, 150)

    mix_alpha = nodes.new('ShaderNodeMixShader')
    mix_alpha.label = 'Alpha cutout'
    mix_alpha.location = (450, 50)

    group_out = nodes.new('NodeGroupOutput')
    group_out.location = (650, 50)

    links.new(group_in.outputs['Base Color'], diffuse.inputs['Color'])
    links.new(group_in.outputs['Base Color'], refraction.inputs['Color'])
    links.new(group_in.outputs['IOR'], refraction.inputs['IOR'])
    links.new(group_in.outputs['Specular Tint'], glossy.inputs['Color'])
    links.new(group_in.outputs['Roughness'], glossy.inputs['Roughness'])
    links.new(group_in.outputs['IOR'], fresnel.inputs['IOR'])

    links.new(group_in.outputs['Diffuse'], has_base.inputs[0])
    links.new(group_in.outputs['Transmission'], has_base.inputs[1])
    links.new(has_base.outputs['Value'], fresnel_or_one.inputs['Factor'])
    links.new(fresnel.outputs['Fac'], fresnel_or_one.inputs['B'])
    links.new(group_in.outputs['Specular'], spec_fac.inputs[0])
    links.new(fresnel_or_one.outputs['Result'], spec_fac.inputs[1])

    links.new(group_in.outputs['Transmission'], mix_base.inputs['Fac'])
    links.new(diffuse.outputs['BSDF'], mix_base.inputs[1])
    links.new(refraction.outputs['BSDF'], mix_base.inputs[2])

    links.new(spec_fac.outputs['Value'], mix_spec.inputs['Fac'])
    links.new(mix_base.outputs['Shader'], mix_spec.inputs[1])
    links.new(glossy.outputs['BSDF'], mix_spec.inputs[2])

    links.new(group_in.outputs['Emission Color'], emission.inputs['Color'])
    links.new(group_in.outputs['Emission Strength'], emission.inputs['Strength'])
    links.new(mix_spec.outputs['Shader'], add_emission.inputs[0])
    links.new(emission.outputs['Emission'], add_emission.inputs[1])

    links.new(group_in.outputs['Alpha'], mix_alpha.inputs['Fac'])
    links.new(transparent.outputs['BSDF'], mix_alpha.inputs[1])
    links.new(add_emission.outputs['Shader'], mix_alpha.inputs[2])

    links.new(mix_alpha.outputs['Shader'], group_out.inputs['BSDF'])

    return ng


def find_group_node(material):
    if not material.use_nodes:
        return None
    for node in material.node_tree.nodes:
        if node.type == 'GROUP' and node.node_tree is not None and node.node_tree.name == NODE_GROUP_NAME:
            return node
    return None


def _find_output_node(node_tree):
    for node in node_tree.nodes:
        if node.type == 'OUTPUT_MATERIAL' and node.is_active_output:
            return node
    return None


def _copy_input(node_tree, group_node, group_socket_name, principled, principled_socket_name):
    src = group_node.inputs[group_socket_name]
    dst = principled.inputs[principled_socket_name]
    if src.is_linked:
        node_tree.links.new(src.links[0].from_socket, dst)
    else:
        dst.default_value = src.default_value


def _push_principled_proxy(material):
    """Rewires the material's output to an equivalent Principled BSDF for export.

    Returns state for _pop_principled_proxy, or None if the material does not use the
    Biomeinator BSDF group.
    """
    group_node = find_group_node(material)
    if group_node is None:
        return None
    node_tree = material.node_tree
    output = _find_output_node(node_tree)
    if output is None:
        return None

    surface = output.inputs['Surface']
    orig_from_socket = surface.links[0].from_socket if surface.is_linked else None

    hasDiffuse = bool(group_node.inputs['Diffuse'].default_value)
    hasSpecular = bool(group_node.inputs['Specular'].default_value)
    hasTransmission = bool(group_node.inputs['Transmission'].default_value)
    specularOnly = hasSpecular and not hasDiffuse and not hasTransmission

    principled = node_tree.nodes.new('ShaderNodeBsdfPrincipled')
    principled.name = 'biomeinator_export_proxy'

    # metallicFactor 1 marks the material specular-only for the loader; everything else
    # exports as a dielectric. The loader reads the specular tint only from
    # KHR_materials_specular's specularColorFactor, so the tint always goes through the
    # Principled Specular Tint socket.
    _copy_input(node_tree, group_node, 'Specular Tint' if specularOnly else 'Base Color',
                principled, 'Base Color')
    principled.inputs['Metallic'].default_value = 1.0 if specularOnly else 0.0
    roughnessInput = group_node.inputs.get('Roughness')
    principled.inputs['Roughness'].default_value = roughnessInput.default_value if roughnessInput is not None else 0.0
    principled.inputs['Transmission Weight'].default_value = 1.0 if hasTransmission else 0.0
    # Specular IOR Level 0.5 is the exporter's specularFactor 1 (extension omitted);
    # 0 exports specularFactor 0, which the loader reads as "no specular lobe".
    principled.inputs['Specular IOR Level'].default_value = 0.5 if hasSpecular else 0.0
    if hasSpecular:
        _copy_input(node_tree, group_node, 'Specular Tint', principled, 'Specular Tint')
    _copy_input(node_tree, group_node, 'IOR', principled, 'IOR')
    _copy_input(node_tree, group_node, 'Emission Color', principled, 'Emission Color')
    _copy_input(node_tree, group_node, 'Emission Strength', principled, 'Emission Strength')
    _copy_input(node_tree, group_node, 'Alpha', principled, 'Alpha')

    node_tree.links.new(principled.outputs['BSDF'], surface)

    return (material, principled, orig_from_socket)


def _pop_principled_proxy(state):
    material, principled, orig_from_socket = state
    node_tree = material.node_tree
    node_tree.nodes.remove(principled)
    if orig_from_socket is not None:
        output = _find_output_node(node_tree)
        node_tree.links.new(orig_from_socket, output.inputs['Surface'])


def _make_material_factors_explicit(filepath):
    """The glTF exporter omits spec-default factors (e.g. roughnessFactor 1.0), so a rough
    material can silently depend on the loader's defaults; write them explicitly instead."""
    import json
    with open(filepath, 'r', encoding='utf-8') as f:
        data = json.load(f)
    for mat in data.get('materials', []):
        pbr = mat.setdefault('pbrMetallicRoughness', {})
        pbr.setdefault('metallicFactor', 1.0)
        pbr.setdefault('roughnessFactor', 1.0)
    with open(filepath, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent='\t', separators=(',', ':'))


def export_gltf(filepath):
    proxies = []
    for material in bpy.data.materials:
        state = _push_principled_proxy(material)
        if state is not None:
            proxies.append(state)
    try:
        bpy.ops.export_scene.gltf(filepath=filepath, export_format='GLTF_SEPARATE', export_keep_originals=True)
    finally:
        for state in reversed(proxies):
            _pop_principled_proxy(state)
    _make_material_factors_explicit(filepath)


class BIOMEINATOR_OT_export_gltf(bpy.types.Operator):
    bl_idname = 'biomeinator.export_gltf'
    bl_label = 'Export Biomeinator glTF'
    bl_description = 'Export glTF with Biomeinator BSDF materials mapped to the fields the engine loads'

    filepath: bpy.props.StringProperty(subtype='FILE_PATH')
    filter_glob: bpy.props.StringProperty(default='*.gltf', options={'HIDDEN'})

    def execute(self, context):
        export_gltf(self.filepath)
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class BIOMEINATOR_OT_add_bsdf_material(bpy.types.Operator):
    bl_idname = 'biomeinator.add_bsdf_material'
    bl_label = 'New Biomeinator Material'
    bl_description = 'Create a material using the Biomeinator BSDF node group and assign it to the active object'

    def execute(self, context):
        ng = ensure_node_group()
        mat = bpy.data.materials.new('Biomeinator Material')
        mat.use_nodes = True
        nodes = mat.node_tree.nodes
        nodes.clear()
        group = nodes.new('ShaderNodeGroup')
        group.node_tree = ng
        group.location = (0, 0)
        output = nodes.new('ShaderNodeOutputMaterial')
        output.location = (300, 0)
        mat.node_tree.links.new(group.outputs['BSDF'], output.inputs['Surface'])
        if context.active_object is not None and context.active_object.type == 'MESH':
            context.active_object.data.materials.append(mat)
        return {'FINISHED'}


def _export_menu_draw(self, context):
    self.layout.operator(BIOMEINATOR_OT_export_gltf.bl_idname, text='Biomeinator glTF (.gltf)')


classes = (
    BIOMEINATOR_OT_export_gltf,
    BIOMEINATOR_OT_add_bsdf_material,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(_export_menu_draw)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_export_menu_draw)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == '__main__':
    register()
