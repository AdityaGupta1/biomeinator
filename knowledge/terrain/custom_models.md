_Last edited: 2026-09-08_

# Custom decorator models

`block_model.h/cpp` loads geometry once and caches four quarter-turn vertex arrays
with shared local indices. `Chunk::createInstances` chooses a variant, translates
its vertices, rebases its indices, and appends triangle metadata to the ordinary
terrain instance. Chunk BLAS/upload ownership remains unchanged. The immutable CPU
cache can be read concurrently without locks and survives chunk unload/reimport.
Only block-system initialization resets it, before workers start.

GLB is the authoring interchange format, not a new scene-loading path. The importer
flattens hierarchy transforms, inverse-transforms normals, reverses winding under
mirrors, and packs normals/UVs into the existing Vertex layout. Materials are
intentionally supplied by the block JSON; one atlas can be swapped independently
of the mesh. Invalid custom assets fail startup rather than silently becoming cubes.

The contract is static uncompressed triangles, NORMAL and TEXCOORD_0, UVs within
0–1, and bounds within one block: X/Z in [-0.5,0.5], Y in [-0.125,1]. The origin is at
the center of the base. Placement adds (0.5,0,0.5) to the block position. Horizontal geometry stays within the owning voxel. The downward allowance is for
buried decorator bases on solid floors, not geometry intended to be visible in the
voxel below. This keeps floor-mounted crystals as intact tilted rectangular prisms.
Export from Blender with Y-up conversion, excluding the prototype studio and its
parent presentation offsets. Preserve component transforms relative to the asset
root (`root.matrix_world.inverted() @ child.matrix_world`); stripping all object
transforms silently loses Object Mode edits. The runtime needs only the GLBs,
texture PNGs/aux PNGs, and block JSONs under `assets/blocks`, not Blender sources.

`randomRotationY` chooses from distinct degrees 0/90/180/270, defaulting to 0.
World seed and all three integer block coordinates choose a turn, using a salt
independent of foliage jitter. Orientation is reproducible across remeshing and
world imports without adding block-state storage. Quarter turns use sign/swap
operations before normal packing, not trig in the worker loop.

Opaque atlases are validated before meshing on all GPUs, including without OMM
support. Custom UVs cannot reuse the pair of OMMs baked for full-tile quads.
Their triangles append FULLY_OPAQUE indices when sharing an OMM-linked chunk.
Supporting cutout models later needs per-model UV-aware micromaps or a separate
geometry path; do not feed them to the existing quad OMM helper.

The glowshroom atlas reserves the left half for caps and the right half for stems.
Its aux R mask makes caps emissive, but its block does not register area lights.
No diffuse transmission is enabled.

## Mushroom authoring decisions

One Blender unit is one block. Use a single opaque 16×16 atlas with nearest
filtering and roughly 16 texels per unit along each surface. Pixel grids on tilted
stem segments rotate with the geometry, rather than remaining world-aligned.
Straight and tilted stem segments have integer pixel lengths; the triangular
elbow occupies one texel. The base must touch Z=0 in Blender (Y=0 after export).
UV islands may overlap; they must stay inside the atlas.

The approved glowshroom is one outward-leaning 5×5 cap flanked by two distinct
outward-leaning 3×3 caps. Cap tops use subtly randomized sprite-palette colors;
side contrast is reduced while retaining average brightness. The brown mushroom
uses a box stem and two box cap layers. These are art direction, not importer
restrictions. The glowshroom aux mask is opaque, R=255 for atlas columns 0–7 and
R=0 for columns 8–15, with G/B=0. Keep cap UVs in the left half and stem UVs in
the right half when editing; `markAsEmitter=false` deliberately excludes these small
lights from explicit sampling without disabling emissive ray hits.

The runtime PNGs are the authoritative final textures, including manual GIMP
edits. To edit the mushrooms, import their runtime GLBs into Blender and assign
the corresponding PNGs for preview. Export selected model objects as GLB with
normals, UVs and Y-up conversion, without animation or compression. Materials
are optional for authoring and ignored by the terrain importer. GLBs retain
geometry, UVs and normals, but not the original untriangulated topology or studio.
The prototype folder was removed intentionally; no generator or source blend is
required to maintain these assets. Preserve the approved atlases when re-exporting.
For Blender preview renders on the development GPU, use Cycles OptiX with CPU
devices disabled.

Crystal shards reuse the opaque `crystal_core` atlas and its emission mask, with
`proceduralColor` enabled to apply the same world-space color ramp as crystal cores.
They do not register as sampled area lights. UV density is 16 square texels per
block along each prism surface, including when tilted.
