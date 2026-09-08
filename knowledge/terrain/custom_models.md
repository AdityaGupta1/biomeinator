_Last edited: 2026-09-07_

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
0–1, and bounds within one block: X/Z in [-0.5,0.5], Y in [0,1]. The origin is at
the center of the base. Placement adds (0.5,0,0.5) to the block position. Keeping
geometry within the owning voxel preserves existing segment-culling assumptions.
Export from Blender with Y-up conversion, excluding the prototype studio and its
parent presentation offsets; `blender/mushroom_prototypes/export_block_models.py`
reproduces the current assets and emission mask.

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
No diffuse transmission is enabled. CPU checks are available via the explicit
`BlockModelTests` target (loader errors, hierarchy, winding, rotations, pixel
assets, cache, and the decorator neighbor-face exception).
