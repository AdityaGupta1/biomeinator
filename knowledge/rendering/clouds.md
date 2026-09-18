_Last edited: 2026-09-17_

# Clouds

Clouds are one scrolling horizontal grid of constant-density boxes. Three octaves of
seeded 2D value noise choose occupied cells. Noise uses world cell coordinates, without
a tiled texture or modulo wrapping. Coverage is a threshold control, not a guaranteed
fraction of occupied cells in a particular view. The retained `cloudPeriod` CLI name
controls pattern scale, not a repeating period. Wind translates the entire grid;
occupancy does not evolve independently or snap with the camera.

The default layer starts at 3,000 blocks and is 256 blocks thick. Cells are 512 blocks
wide, with an 8,192-block pattern scale and coverage control 0.3. The higher base was
requested after viewing the initial block-cloud captures at 1,500 blocks.

The earlier Blender-shaped smooth clouds have been replaced. The user's
`blender/clouds/clouds_ref.blend` remains a reference asset, but its noise graph and mesh
scale no longer drive this renderer. Erosion, height ramps, coefficient caching and
step-size settings have been removed.

## Traversal and lighting

`cloud_traversal.hlsli` clips each actual ray segment to the layer and draw distance,
then advances through horizontal cell boundaries with 2D DDA. This includes rays that
end at geometry, rays originating inside the layer and vertical rays. Adjacent occupied
cells become a single continuous interval. There is no fixed iteration budget that can
exhaust itself before reaching distant clouds.

Each occupied interval attenuates by `exp(-extinction * length)`. Its configured 1–32
lighting samples (default 1) estimate only in-scattering; changing their count or RNG
does not change opacity. Samples are stratified over the truncated exponential within
the interval, so even a very long opaque cloud samples its visible skin. Sun-disk
directions remain stochastic. View integration stops below transmittance 0.002.

Sun, surface-light and fog visibility queries use the same occupied boxes and analytic
lengths. They retain the finite shadow travel limit after entering the layer, as well
as the draw-distance limit from the query origin. This bounds near-horizontal shadow
cost without shortening viewing rays. Optical-depth queries stop above depth 12.
The shadow limit deliberately omits more distant attenuation; it is a performance
approximation, as are the ambient term and multiple-scattering model.

Ambient and multiple scattering are enabled independently by default, with strength
controls. Multiple scattering adds two softer exponential attenuation terms using
the same sun optical depth; it does not launch another path bounce. There is no powder
correction or cloud-specific aerial color blend. Foreground fog/water attenuate cloud
radiance, while endpoint throughput receives cloud attenuation once. Existing analytic
fog ambient lighting does not resolve cloud/fog overlap. Clouds apply only in voxel mode.

## Occupancy map

A 512 x 512 R32_UINT texture stores exactly one occupancy value per world grid cell
around the advected camera. Integer loads do not interpolate or change cloud shape.
Outside this window, the identical noise function evaluates occupancy directly.
Crossing the map boundary therefore does not clip clouds or repeat the pattern.
The map is rebuilt each enabled frame before tracing (1 MiB, one compute dispatch),
without history, cached lighting or a separate invalidation state machine.

The minimum cell size and finite draw/shadow distances bound traversal cost. Extremely
small cells with long draw distances are more expensive, especially outside the map.
Cloud LODs are not implemented yet.

## SER

`applySegmentAtmosphere` searches for the first occupied interval after geometry
tracing, retaining both that interval and the DDA continuation. An optional one-bit
`NvReorderThread` hint groups cloud hits before lighting. Misses return immediately;
hits resume without searching empty space again. This also covers primary G-buffer
segments and ordinary bounces. Doing classification only in the miss shader would
incorrectly exclude clouds in front of geometry. Existing surface SER remains separate.
The extra cloud reorder defaults off: it added 0.07–0.09 ms in the measured lower-export
views at one lighting sample. It remains available for testing other views/sample counts.

## DLSS albedo

Sky misses and first perfect-specular sky reflections blend unshadowed cloud color
with sky color using the rendered segment's deterministic analytic transmittance.
No separate guide march is needed. The guide uses sun-centre phase and atmospheric
energy at the layer midpoint; it never uses sampled lighting or cloud self-shadowing.
Thus increasing lighting samples or changing their RNG cannot add noise to this mask.
Camera jitter, animated wind and stochastic geometry visibility remain separate.
Cloud depth and motion are not supplied as separate reconstruction guides.

## Validation

Builds and numerical/image/performance checks are recorded in `build/block-clouds/`.
The comparison uses the user's lower and upper exports from 2026-09-12. The old smooth
executable is saved beside the main executable as `Biomeinator-smooth-baseline.exe`.
Scene-specific measurements compare the two different cloud styles, not equal images.

On RTX 4070 SUPER, 960 x 540 output (557 x 313 internal), DLSS, SHaRC/FG off, path depth 4,
200 measured frames after warmup, animated from time 200 (day) or 0 (horizon):
These measurements use the initial 1,500-block base height.

| Lower-export view | Smooth baseline | Blocks, cloud SER off | Clouds off |
|---|---:|---:|---:|
| Day | 4.54 ms | 1.97 ms | 1.79 ms |
| Near horizon | 6.51 ms | 2.57 ms | 2.37 ms |

The occupancy dispatch costs 0.013 ms, versus 0.215 ms for the old horizontal field.
With cloud SER enabled the block version measured 2.05 / 2.66 ms. The path-trace scope
accounts for the added reorder cost; scene-update timings stayed similar.
After raising the default base to 3,000 blocks, the same day/horizon runs measured
1.98 / 2.75 ms. The final raised-height capture is `build/block-clouds/raised.png`.

An independent GPU brute-force box-intersection reference agreed with DDA occupied
lengths for 256 rays within 0.00000382 blocks. Cases include vertical/horizontal rays,
negative cells, exact boundaries/corners, inside-layer origins and finite endpoints.

One-frame sky/cloud albedo crops (jitter disabled) are pixel-identical across RNG seeds
1 and 1234 and across lighting sample counts 1 and 32. Captures include below/above/
inside views, sunset, water reflection, ambient/multiple-scattering toggles, and DLSS
with SHaRC enabled. Still captures do not establish flicker-free moving reconstruction.
The cloud-disabled water diffuse-albedo regression passed its 0.001 threshold with
RMSE 0.000620. Startup/reset defaults and export options agree for all 19 cloud settings.
