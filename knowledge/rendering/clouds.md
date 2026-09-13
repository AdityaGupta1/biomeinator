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
attenuation, to 9,000 blocks after layer entry. These queries also stop at the draw-distance
limit measured from their origin and at the horizontal field texture's border. Near the
horizon, rays from terrain/fog can first reach the nominal layer hundreds of kilometres
away, outside both caches. Evaluating the procedural field there dominated sunrise cost.
Shadows outside the local field are now deliberately omitted; view-ray density still has
its procedural fallback. No distant-cloud clipping is reintroduced on viewing rays.
Grazing transmittance queries widen their spacing towards one sixth of the interval,
blending back to the configured shadow spacing by |direction.y| = 0.15. This bounds the
cost of long, mostly empty horizontal shadow intervals without changing view coverage.
Both shadow and rendered view sample positions use the path RNG. Deterministic sampling
is confined to the separate albedo guide; using it for rendered fine erosion causes
banding and apparent shape changes when the camera moves, even with animation paused.
Transmittance terminates near opacity.
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

## DLSS albedo

Sky misses and first perfect-specular sky reflections use unshadowed cloud color as their
guide. A separate deterministic base-density march supplies its opacity mask, omitting
fine erosion and all shadow queries. It averages adjacent endpoint densities and grows
spacing with distance, bounded by horizontal feature size and vertical layer crossing
distance. It covers the full viewing interval rather than the shorter shadow budget.
That opacity blends unshadowed cloud color with the dome color before Reinhard
mapping into the existing guide range. Cloud color uses ambient sky and the sun-centre phase
and atmospheric energy at the layer midpoint, including the enabled multiple-scattering
approximation at zero optical depth. It does not include cloud self-shadowing or foreground
fog. The actual radiance still uses stochastic sun-disk samples and shadowing.

Do not derive this guide from `CloudResult.radiance` or its stochastic transmittance:
these contain sample noise that DLSS treats as surface detail. Conversely, do not make
the rendered view density deterministic to reuse it for the guide: fine erosion aliases
against that sampling grid. The guide intentionally approximates the silhouette without
fine detail, allowing a cheaper march while keeping radiance sampling independent.
Camera jitter, animated field changes and any stochastic geometry visibility remain separate from cloud
sampling noise. Primary sky guides belong only to path split 0, since collect sums splits.

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

The next change bounded local shadow queries, coarsened grazing shadow intervals, and
made view opacity deterministic for unshadowed DLSS albedo. Against `8fb60d1`, using the
same exports/GPU/resolution, SHaRC/FG off, and 100–300 measured frames:

| View/time | Before | After |
|---|---:|---:|
| Lower, sun on horizon (paused time 0) | 8.14 ms | 3.80 ms |
| Upper, sun on horizon (paused time 0) | 6.42 ms | 5.80 ms |
| Lower, daytime (animated from time 200) | 2.74 ms | 3.12 ms |

Artifacts for that intermediate implementation use the `trapezoid` suffix. With jitter disabled,
the sky region of one-frame albedo captures matched exactly across RNG seeds 0 and 1234.
Sunrise and DLSS captures check the fixed density sampling and coarser shadow approximation.
Subsequent user testing found that deterministic rendered density still caused fine-detail
banding and apparent mutation during vertical camera motion. It has been replaced by
jittered rendered samples and an independent deterministic base-density albedo guide.
Fixed quadrature trades sampling noise for possible discretization bias; unusually large
step settings can still miss thin features. Distant shadows outside the local field are
deliberately absent. The cloud-disabled water albedo regression remained at RMSE 0.000620.

The correction uses jittered view samples and the separate no-erosion albedo guide.
`splitguide_refined*` artifacts verify identical sky/cloud albedo pixels when changing
RNG seed 0 to 1234 and fine strength 0.05 to 0.3. Paused-time captures at camera heights
20 blocks below and above the lower export check visible cloud detail without the fixed
fine-sampling bands; these are discrete views, not a continuous motion test. The final
DLSS capture also enables SHaRC. Matching 200-frame measurements gave 3.27 ms for lower
daytime, 4.20 ms for lower sunrise and 6.34 ms for upper sunrise. This adds about 0.15,
0.40 and 0.54 ms respectively over the deterministic-rendered-density version. The guide
uses finer vertical spacing than the initial coarse mask to avoid stripes in its silhouette.

The diffuse-albedo, fractional-opacity albedo and water-reflection albedo regressions
passed their existing thresholds with clouds disabled (RMSE 0.000374, 0, 0.000620).
Finite cloud/geometry overlap and SHaRC-enabled captures also completed. Golden images
and the user's edited Blender file were not modified.
