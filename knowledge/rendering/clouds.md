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
The last interval is shortened to the endpoint. `cloudMarchDistance` caps travel inside
the layer; `cloudMaxDistance` caps viewing distance. Solar rays do not use the viewing
cutoff, so a distant layer entrance does not suddenly remove shadows. They still have
the inside-layer travel cap. Stratified sample positions use the path RNG; transmittance
terminates near opacity. These are finite-step, finite-distance approximations.

Cloud in-scattering is added separately from the attenuated dome light, so sun MIS never
weights cloud in-scattering. Each occupied scattering sample chooses one random sun-disk
direction and uses it for phase, atmosphere transmittance and the base-density shadow march.
The multiple-scattering checkbox adds two softened attenuation terms using that same
optical depth; it is not a traced extra bounce. Ambient cloud lighting remains approximate.
There is no powder correction, cloud-specific aerial blend, screen-space cloud buffer,
cached density, or cached lighting.

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

## Validation

The RelWithDebInfo build includes reference, SHaRC update and SHaRC query shader variants.
GPU evaluation of 256 sample points matched the edited Blender graph's pre-extinction
density within 0.000026, with 42 nonzero samples including the bottom transition.
Direct versus cached GPU evaluation with evolving noise and outside-window fallback
agreed within 0.00000045. Numerical scripts, output and scene captures are in
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

The diffuse-albedo, fractional-opacity albedo and water-reflection albedo regressions
passed their existing thresholds with clouds disabled (RMSE 0.000374, 0, 0.000620).
Finite cloud/geometry overlap and SHaRC-enabled captures also completed. Golden images
and the user's edited Blender file were not modified.
