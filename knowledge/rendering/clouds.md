_Last edited: 2026-09-17_

# Clouds

Clouds are one horizontal layer of constant-extinction boxes on a 2D grid, chosen by
thresholding three octaves of seeded value noise. Boxes rather than a smooth density field
because the block look matches the terrain and, more importantly, every transport quantity
becomes analytic per occupied run of cells: no ray marching, no step-size tuning, and opacity
that is independent of sample count and RNG. Voxel mode only.

The noise is evaluated in world cell coordinates, so the pattern never tiles. Wind translates
the whole grid (`cloudPosition` removes the translation before lookup); cells never change
shape on their own. The translation is `wind * animTime`, which would drift past float
precision within a day of animation, so the CPU computes it in double and ships it split into
integer blocks plus a fraction, the same fixed-point emulation the camera position uses.
Coverage is a threshold on the noise, not a guaranteed occupied fraction.

## Occupancy map

`cloud_occupancy.cs.hlsl` rebuilds a `CLOUD_OCCUPANCY_MAP_SIZE`² R32_UINT window of cells
around the camera every frame that clouds are enabled (one integer per cell, no filtering).
Traversal reads the map inside the window and falls back to evaluating the noise directly
outside it, so leaving the window neither clips nor repeats clouds. The rebuild is cheap
enough that there is deliberately no caching or invalidation logic.

## Traversal

`cloud_traversal.hlsli` clips a ray to the layer's height range, the draw distance and the
caller's segment length, then walks cell boundaries with a 2D DDA. `nextCloudInterval`
merges adjacent occupied cells into one interval, so a long solid bank costs one exponential.
Rays that start inside the layer, end at geometry or run vertically all go through the same
path. There is no fixed iteration budget; cost is bounded by cell size and draw distance.

`beginCloudTraversal` also takes a limit on travel *inside* the layer (`layerDistance`).
View rays pass it unbounded; shadow queries pass `cloudShadowDistance`. The asymmetry is a
performance approximation: a near-horizontal shadow ray would otherwise cross the entire draw
distance, whereas view rays must reach distant clouds or they visibly pop.

## Lighting

Each interval's transmittance is `exp(-extinction * length)`. The configured lighting samples
estimate only in-scattering. They are stratified over the interval's extinction-weighted depth
(inverse-CDF of the truncated exponential), so an opaque bank still samples its lit skin
instead of its interior. Each sample draws one sun-cap direction, reused for the phase term,
atmospheric attenuation and cloud self-shadowing. Multiple scattering is two extra
exponential terms on the same sun optical depth (`cloudSunScattering`), not extra bounces;
the ambient term is the zenith sky color scaled by a setting.

`applySegmentAtmosphere` integrates clouds and fog on the same segment.
Cloud in-scatter is attenuated by fog and water in front of it, and the fog march attenuates
its own samples by clouds, so the two media compose without double-counting. In-scatter is
computed only at path depths ≤ 1, matching fog; deeper bounces get transmittance only.

Sun NEE, area-light NEE and the fog march all multiply by `cloudTransmittance` along their
existing rays. This is where clouds shadow the ground.

## DLSS guides

The G-buffer overrides depth and motion with the first cloud boundary in front of the
endpoint regardless of opacity, and un-applies wind for the previous position so pausing
animation freezes wind motion but not camera motion. Starting inside a cloud, the exit
boundary is used; a geometry endpoint or the draw distance is never reported as a surface.

Sky misses (and first perfect-specular sky reflections) blend an unshadowed cloud color with
the sky using the segment's analytic transmittance (`cloudGuideColor`). The guide must never
use the sampled lighting or self-shadowing: any noise there would be reconstructed as
detail. See [dlss.md](dlss.md#cloud-albedo).
