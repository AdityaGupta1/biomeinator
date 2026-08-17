_Last edited: 2026-08-16_

# Terrain Opacity Micromaps

`src/terrain/terrain_omm.h/cpp` — bakes 2-state OMMs for the terrain texture's alpha-cutout
slices so terrain never invokes anyhit (voxel mode + raytracing tier 1.2, see
`Renderer::getUseOmms()`; anyhit path remains as the fallback).

## Why 2-state OMMs are exact, not approximate

Every cutout slice's mip-0 alpha is strictly binary (asserted at bake), and every terrain
face uses the same corner UVs and `(0,1,2),(0,2,3)` quad split, so a subdivision-level-4 OMM
puts each micro-triangle entirely inside exactly one texel of the 16×16 tile — including for
the quad's second triangle, whose barycentric→UV map is a shear (micro-triangle rows still
land in single texel rows because half-cell offsets stay multiples of a texel). Sampling the
micro-triangle centroid therefore reproduces the voxel-mode point-sampled alpha test
bit-exactly at mip 0. Two OMMs per cutout slice (one per quad triangle) cover every face in
the world.

## Interaction with alpha mips: cutout mips are color-only under OMMs

OMMs have no LOD: traversal always tests the mip-0 pattern, so under OMMs the texture's
alpha channel is redundant for visibility — a delivered hit already means "opaque mip-0
texel". Re-testing alpha at the ray-cone mip in shading would cull a second time with a
*different* pattern: the coverage-preserving binarized mips redistribute coverage per level
(they consolidate partial footprints to solid and zero out sparse ones — GRASS collapses to
fully empty by mip 4), so distant cutout hits sampled alpha 0, path splitting routed them to
passthrough, and distant foliage largely vanished for primary rays. Paths that never read
alpha (secondary bounces, path splitting disabled, NRC update) instead shaded those texels
opaque — occasionally black, where the premultiplied cascade had zero source alpha.

So when `Renderer::getUseOmms()` is set, `LoadTextureOptions::useOpaqueCutoutMips` makes
cutout tiles' lower mips carry color only: the alpha quantization is skipped so the
downsample cascade weights colors by true mip-0 coverage (the quantized cascade distorted
weights), every texel is forced fully opaque, and texels with zero coverage take dilated
neighbor colors. The dilated colors are defensive — voxel mode point-samples and hits only
land over opaque mip-0 texels, so they are never read. The anyhit fallback keeps the
coverage-preserving quantized mips; it still alpha-tests at the cone mip.

Distant foliage therefore resolves to its true subpixel coverage — thinner than the pre-OMM
consolidated-mip look, plus some added shimmer for the denoiser. This was accepted as the
cost of the perf win. A 4-state variant was implemented and measured (unknown micro-tris
where the mip chain disagrees with mip 0, anyhit at silhouettes only, occlusion rays forced
2-state): it restored the old distant look but kept only about half the win (−6% frame time
vs −11% for 2-state on a foliage-heavy world), so it was removed — revive from history if
the distant-foliage look ever matters more.

## Baking and ordering

The bake runs synchronously in `TerrainMaterials::init` (before any chunk meshing task can
exist), so worker threads can read the tables lock-free. The GPU array build is deferred to
the first frame (`buildArrayIfPending` in the renderer's voxel-mode update, before
`Terrain::update`) because material init has no command list; chunk BLASes can't be queued
until chunks generate, which takes several frames, and `AcsHelper` asserts the array VA is
set when a BLAS with OMM indices is built.

The micro-triangle index decode is the reference bird-curve implementation shared by the DXR
and Vulkan specs. Bit packing is LSB-first per byte along the curve. A bake-time assert
checks total opaque micro-triangles == 2 × opaque texels per slice, which catches any curve
or UV-mapping mistake.

## Per-chunk emission (`chunk.cpp`)

Terrain faces emit one R16 OMM index per triangle: `FULLY_OPAQUE (-2 as 0xFFFE)` for opaque
slices, the slice's baked OMM pair for cutout slices. Chunks with zero cutout faces drop the
index buffer entirely and set `Instance::setIsOpaque(true)` instead — plain `OPAQUE`
geometry skips anyhit just as well and avoids the ~2 bytes/tri buffer. Water instances never
get OMM indices; their refit path is untouched.
