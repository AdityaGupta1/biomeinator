_Last edited: 2026-09-12_

# Clouds

The cloud density follows `blender/clouds/clouds_ref.blend`: distorted 2D Smooth F1,
top and bottom shaping, centered 3D detail, and a descending density ramp.
`cloud_density.hlsli` contains the composition; `cloud_reference_noise.hlsli` contains
the Blender noise adaptation. Game altitude, thickness, horizontal scale and extinction
are independent controls. The Blender mesh dimensions, mapping scale and final volume
multiplier are not game-space units. `cloudPeriod` retains its CLI name for compatibility
but now controls horizontal scale; the lattice is unbounded and frequencies do not snap.

## Detail and integration

`clouds.hlsli` marches actual camera/path segments, including segments ending at geometry.
Fine detail is selected by the accumulated ray-cone diameter at each sample. The fixed
8-block cutoff was calibrated from a 60-degree vertical FOV, 720 internal vertical pixels,
and roughly 5 km to the cloud plus a 200-block specular reflection. It is not recalculated
when the camera changes. Diffuse scattering rapidly exceeds it. Refraction uses the existing
cone propagation. The cone passed to the march must be the one at the segment origin,
not its already-advanced width at the next surface hit.

All transmittance-only queries (surface lights, cloud sunlight, fog) omit fine detail.
Centered detail can increase density, so rejecting a point because its base density is
zero would be incorrect. Empty skips bound both the signed noise and Smooth F1 before
concluding that the final ramp is zero.

March spacing is in blocks, with separate detailed-view, broad-view and shadow settings.
Viewing rays cover the whole layer intersection up to `cloudMaxDistance` (100,000 blocks),
including empty space before distant clouds. Do not apply the shadow travel budget to
view rays: a camera inside the nominal layer but above its occupied portion can spend
that entire budget in empty space, clipping cloud tops at grazing angles. Distant view
steps grow with distance, limited by horizontal feature size and the vertical layer
crossing distance; the configured step remains the minimum spacing. There is no fixed
iteration budget. The last interval is shortened to the endpoint.

`cloudMarchDistance` limits transmittance-only queries, including solar shadows and fog
attenuation, to 9,000 blocks after layer entry. Solar rays do not use the viewing cutoff.
Stratified sample positions use the path RNG; transmittance terminates near opacity.
These are finite-step, finite-distance approximations.

Cloud in-scattering is added separately from the attenuated dome light, so sun MIS never
weights cloud in-scattering. Each occupied scattering sample chooses one random sun-disk
direction and uses it for phase, atmosphere transmittance and the base-density shadow march.
The multiple-scattering checkbox adds two softened attenuation terms using that same
optical depth; it is not a traced extra bounce. Ambient cloud lighting remains approximate.
There is no powder correction, cloud-specific aerial blend, screen-space cloud buffer,
or cached lighting.

`applySegmentAtmosphere` handles fog/cloud composition before surface or environment
contributions. Cloud radiance includes foreground fog/water attenuation; endpoint throughput
gets each medium's transmittance once. Fog direct scattering includes base-cloud attenuation
along its view and sun rays. The existing analytic fog ambient term is still an approximation
and does not resolve cloud/fog overlap. Clouds are voxel-mode-only; glTF transport is unchanged.

## Noise coefficient cache

SkyAtmosphere owns one 1024 x 18 RGBA32F texture (288 KiB), rebuilt before tracing when
clouds are enabled. It stores two sets of 1024 noise rotation coefficients and 128 x 128
Voronoi feature points around the camera's advected field position. Both are functions
of the same frame's settings/time. Integer loads preserve the underlying field, apart
from floating-point rounding; nothing is spatially filtered. Cells outside the stored
window are evaluated directly, so moving the window does not change density.

This avoids repeating trigonometry and feature-point generation in every nested march.
It does not store density, optical depth, radiance or history. Disabling clouds performs
no coefficient dispatch. Settings changes use the normal accumulation/SHaRC invalidation;
there is no separate cloud invalidation state machine or resize path.

## Filtered horizontal field

The expensive distorted Smooth F1 field depends only on horizontal position. A second
compute pass evaluates it into a 1024 x 1024 R16F texture (2 MiB) after the coefficient
pass and before tracing. View and lighting samples bilinearly sample this field, then
apply height shaping and, where allowed by the ray cone, procedural fine erosion.
This deliberately approximates the broad shape instead of reevaluating distortion and
Voronoi at every nested raymarch sample. It does not cache final density or lighting.

The texture covers the viewing radius plus the shadow travel margin around the camera.
Its origin snaps to the advected world-space texel grid so moving the camera does not
resample stationary field points. Texels are capped at horizontal scale / 512 to avoid
destroying smaller clouds when the draw distance is large; samples outside the window
fall back to procedural evaluation. Small-scale presets can therefore cost more.
Rebuilding each frame handles wind, evolving noise and settings without temporal history.

The current default horizontal scale is 110,736.695312 blocks, thickness is 3,000 blocks,
and coverage is 0.3. Base altitude remains 1,500 blocks. These double the earlier preset's
dimensions, including the detail proportions, without importing Blender scene units.

## Validation

The RelWithDebInfo build includes reference, SHaRC update and SHaRC query shader variants.
GPU evaluation of 256 sample points matched the edited Blender graph's pre-extinction
density within 0.000026, with 42 nonzero samples including the bottom transition.
Direct versus cached GPU evaluation with evolving noise and outside-window fallback
agreed within 0.00000045 for the coefficient cache alone. The later filtered horizontal
field intentionally does not retain this precision. Numerical scripts, output and scene captures are in
`build/clouds2-validation/`.

The scene captures cover water reflections, below/inside/above the layer, sunset, and
multiple scattering on/off. Still captures do not establish flicker-free animated DLSS;
cloud motion and cloud depth are not provided as separate reconstruction guides.

A 100-frame animated water-reflection test on RTX 4070 SUPER, 960 x 540 output
(557 x 313 internal), SHaRC/FG off and stable power state enabled measured 8.90 ms
median GPU frame time versus 20.77 ms for the initial uncached implementation and
2.17 ms with clouds disabled. The coefficient dispatch was about 0.003 ms. These
are short scene-specific measurements, not a comparison against the old `clouds`
branch or evidence that cloud marching is cheap. Step-size controls remain the
main quality/cost tradeoff.

The subsequent filtered-field and distance-spacing changes were tested against exports
`2026.09.12_20-33-13` (below clouds with terrain) and `2026.09.12_20-33-43` (upper view).
Matched 200-frame animated measurements on the same GPU/output/internal resolution,
SHaRC/FG off, path depth 4 and the enlarged 30%-coverage preset measured:

| Export | Before | After |
|---|---:|---:|
| Lower | 7.53 ms | 2.74 ms |
| Upper | 14.73 ms | 4.16 ms |

The horizontal-field dispatch took 0.213 ms. Captures with the original smaller preset
and a temporarily black primary sky confirmed the missing distant cloud tops reappeared;
the diagnostic shader change was removed. The lower-view comparison with matched new
settings preserved the nearby silhouettes while restoring distant clouds. Reports and
captures are under `build/clouds2-fast/`. Exports do not record animation time; captures
used time 200, so they are not guaranteed to reproduce the user's exact animated frame.
The final lower-export SHaRC run completed at 3.01 ms; disabling clouds measured 1.77 ms
and omitted both cloud compute passes. A second animation-time capture also completed.

The diffuse-albedo, fractional-opacity albedo and water-reflection albedo regressions
passed their existing thresholds with clouds disabled (RMSE 0.000374, 0, 0.000620).
Finite cloud/geometry overlap and SHaRC-enabled captures also completed. Golden images
and the user's edited Blender file were not modified.
