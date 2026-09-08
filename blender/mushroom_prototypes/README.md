# Mushroom prototypes

Open `mushroom_prototypes.blend`. The **Mushroom prototypes** collection contains
editable box meshes under one parent per asset. The other collection is the
preview studio. Textures are packed into the blend and supplied as PNGs.

- Brown: three boxes, 36 triangles, 6/16-block overall height.
- Yellow pair: six rectangular prisms and two triangular bends, 72 triangles
  after removing internal joint faces. Tall stem segments are 3 + 3 pixels;
  small stem segments are 1 + 2 pixels. Upper segments and their pixel grids
  rotate together. Each bend's triangular face occupies a single texel.
- Each asset uses its own single 16×16 atlas, nearest filtering, and UVs within
  0–1. UV islands deliberately overlap to reuse pixels. Atlas colors derive
  from opaque pixels in the corresponding existing sprite. Cap side colors
  have reduced contrast around their original linear-light palette mean.
- UVs use 16 texels per world unit, with square pixels aligned to each
  rectangular prism. All face edge and diagonal lengths are checked against
  UV distances; the maximum relative error is recorded in `validation.json`.
- The yellow cap uses closely spaced golden palette colors for subtle mottling.
- The blend remains a shape/base-color preview. `export_block_models.py`
  exports the models to `assets/blocks/models` and their atlases to
  `assets/blocks/textures`, adding an aux mask for glowshroom cap emission.
  The runtime block definitions use these assets as custom decorators.
- One Blender unit represents one voxel. Parent X offsets separate the models
  for presentation; child geometry is local to each asset, with its base at Z=0.
  The studio ground is also exactly Z=0.

`build_prototypes.py` regenerates the atlases, blend, validation report, and
three preview renders using Blender's background Python interface. `preview.png`
is the main three-quarter view; `front.png` and `rear.png` show other angles.
Rendering and denoising use OptiX on the GPU, with CPU devices disabled.
The script stops if no OptiX device is available rather than falling back to CPU.

Run the builder with `-- --cluster` for the separate three-mushroom proposal in
`cluster/`: one 5×5 cap leaning back, flanked by two 3×3 caps leaning outward.
The small stems differ in length and bend angle. The runtime exporter now uses
this cluster for the glowshroom and the original brown mushroom. It copies the
atlas from disk, preserving GIMP edits; exporting does not regenerate textures.
