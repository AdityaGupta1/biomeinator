# Clouds: implementation and handoff

## Current sun sampling and sky strength (2026-09-12)

Prior cloud/evolution/fog work was committed as `b8e7a0f`. The subsequent
`skyStrength` setting exposes a 0�10� sky multiplier (default 1�) under Atmosphere.
It scales sky radiance and volume ambient, leaving direct solar energy unchanged.

Every explicit solar lighting sample now selects **one random uniform direction
on the solar disk**, through `sky/sun_sampling.hlsli`. Surface NEE, fog, cloud
single scattering, cloud light-cache generation, and both atmosphere scattering
LUT passes use it. The center remains only for disk geometry, PDF support and
LUT coordinates. Camera/BSDF rays that hit the disk retain their actual direction.

Fog traces at most one terrain shadow ray per scattering sample, replacing the
four fixed directions. Phase, planet visibility, atmospheric transmittance, fog
attenuation and cloud attenuation use the same sampled direction. Stratified
random view-march positions remain. Volume sun energy no longer multiplies by
an analytical visible-disk fraction on top of the random visibility estimate.
The old sun-center shadow multiplier on fog ambient was removed.

Direct cloud attenuation now marches the actual selected ray, returning
`exp(-optical depth)` instead of using center-direction cached depth. Light
marches use midpoint density quadrature, are bounded to 12 km inside the layer,
and stop once optical
depth reaches 8 (transmittance below 0.00034). Layer entry is independent of the
view draw-distance cutoff. `cloudLightSteps` controls density evaluations along
that single ray; the UI now calls it "Sun ray march steps". The spatial optical
depth cache remains only for the approximate higher scattering orders and is
built with one random disk ray per texel whenever it refreshes. That cache uses
midpoint density quadrature to avoid baking spatial integration noise into it.

Validation: all shaders and RelWithDebInfo built. A GPU test of 1,048,576 samples
checked direction normalization, cap support, radial/azimuth distribution, and
sunset horizon visibility; maximum visible-fraction error was 0.0022. Animated
1280�720 runs in the user's world (launch time 585, SHaRC/frame generation off,
max depth 4, 32 measured frames) measured median GPU frame times of 12.02 ms
before and 13.60 ms after. These short animated runs are indicative, not a
controlled temporal-quality comparison. Direct cloud marching adds work despite
reducing fog terrain rays. Reports and captures (`final.png` is the revised density quadrature) are in
`build/sun-sampling/`. Static captures cannot establish flicker-free animation.

## Current: Blender reference and user tuning (2026-09-12)

The user supplied `blender/clouds/clouds_ref.blend` and requested its basic
shape, preserving Smooth F1 and intentionally flat bottoms. Their latest
instruction is to expose controls and let them refine the appearance in game.
That implementation replaces the earlier weather/erosion/taper model below.

See `blender/clouds/README.md` for the exact graph, scaling, implementation,
and validation. Fourteen graph parameters are exposed in Atmosphere → Cloud
settings, with CLI, reset, export, and cache invalidation support. The old
shape CLI options have been replaced. Defaults reproduce the Blender graph;
tile size is 34285.714 m and density 0.006666667/m for a 1500 m layer.

Build and numeric reference comparison passed. Top/bottom reference and engine
captures plus changed-parameter smoke checks are under `build/cloud-reference/`.
Final appearance and performance are not established; leave visual tuning to
the user. Prior fog fixes and bounded marching remain in place.

## PC follow-up: cloud shape and sunset fixes (2026-09-12)

The sections below describe the earlier implementation and its historical
constraints. This follow-up uses the user's supplied 3000–4500 m, 50% coverage,
0.015/m density, 128-step preset as the launch and GUI-reset defaults.

- View integration now caps travel **after layer entry** at six layer thicknesses,
  capped at 12 km (9 km for this preset). The draw-distance setting still limits
  entry/end distance from the ray origin. Existing termination below 0.002
  transmittance remains. Sampling offsets no longer change every frame.
- Weather/billows use periodic domain distortion; the spatial cache and erosion
  share a rotated horizontal basis, including the inverse transform when building
  sunlight. The model is still periodic, but no longer aligned to cardinal axes.
- Height-independent column summits and increasing occupancy thresholds narrow
  upper cloud lobes, in addition to the existing bottom/top density fades.
- The sunlight cache now integrates toward either layer exit, continuously across
  sun elevation zero, rather than resetting optical depth to zero below it.
- Fog and cloud illumination use fractional solar-disk visibility and valid-domain
  atmospheric transmittance. Fog evaluates this at the scattering height, uses
  stable midpoint integration, and averages four terrain shadow directions.
  This increases fog shadow-query cost; performance has not been benchmarked.

Validation: all shaders and RelWithDebInfo built successfully using the documented
environment cleanup plus single-node MSBuild with node reuse disabled. Runtime
captures in `build/cloud-fixes/` exercise the supplied defaults below the layer,
inside it at 3250 m, and just after sunset. Captures use 960×540, 32 accumulated
frames, SHaRC/frame generation off, max path depth 4. Static captures do not
establish animated DLSS quality or eliminate all temporal artifacts. The existing
post-capture shutdown stall persists; only owned capture processes were stopped.

Remaining approximations: hard integration range, periodic finite-resolution
fields, four-point terrain penumbra, and no cloud integration on finite geometry
segments. These fixes do not implement anvil clouds or a new cloud temporal filter.

Updated 2026-09-12. Work is on branch `clouds`, pushed to `origin/clouds`.
Implementation commit: `d5bb7ee` (this handoff is a subsequent documentation commit).
The user is moving development/testing to a PC with an RTX 4070.

## User intent and constraints

- Realistic volumetric clouds, with broad masses, medium billows, and fine erosion.
  The user rejected both the initial soft appearance and a later uniform, visibly
  repeating fine-detail fringe. Appearance is still being tuned, not accepted.
- Default coverage around 30%, with ample clear sky.
- Clouds must stay independent of camera motion, move with wind, block the sun,
  cast ground shadows, and appear in reflections. Flying through them is unimportant.
- Respect the real-time hardware-raytracing/DLSS-RR budget. Primary and sharp
  reflection rays deserve more detail; diffuse/rough rays and fog may be cheaper.
- Avoid a shared directional environment approximation for now. Spatial density
  and sunlight caches are implemented instead.
- Expose many controls so the user can determine the desired shape themselves.
- The user asked about towering cumulonimbus. We explained the extra work below;
  it has not been implemented or selected as the next development task.
- Latest execution preference: **do not launch the app; the user will run it on
  their 4070**. No app launches occurred after that instruction. Let the user
  authorize further automated runtime testing on the destination machine.

## What is implemented

1. A world-space, periodic, flat cloud layer, default 650–1100 m altitude and
   4096 m horizontal period. Global origin rebasing is accounted for. Wind is
   based on animation time (default X/Z speeds 8/3 m/s).
2. One-time 64³ RGBA16F periodic noise generation: three-octave gradient Perlin
   fBM plus three cellular scales. Broad weather, medium Perlin/Worley billows,
   medium erosion and domain-warped fine erosion replace the original expensive
   procedural noise evaluated at every march sample.
3. A 128×32×128 R16F shape envelope rebuilt on settings changes. Erosion only
   removes density, so empty envelope samples can skip detail work.
4. A matching spatial optical-depth-to-sun volume, refreshed on animation time
   or relevant settings changes. Runtime illumination and ground shadows use
   filtered lookups instead of nested light marches.
5. Jittered view marching with analytic integration per step and early opacity
   termination. Lighting includes approximate multiple scattering, ambient sky,
   phase/powder controls and approximate distance haze.
6. A primary-camera cloud compute pass, default half **internal render**
   resolution. It stores radiance/transmittance, bilinearly sampled only at sky
   misses. The sun disk remains full resolution. This is not an environment map
   used to light surfaces.
7. Secondary misses still march from actual ray origins. Ray-cone angle blends
   between the main sample count (default 32) and broad-ray count (default 8).
   Sharp reflections keep more samples. This is sample-count LOD, not a fully
   footprint-filtered density hierarchy.
8. Solar next-event estimation uses spatial transmittance. Escape-ray MIS weights
   solar radiance separately from cloud in-scattering. Fog sun lighting uses
   the spatial shadow model; fog ambient remains an approximation.
9. CPU/HLSL settings, resource ownership, descriptor/root-signature integration,
   resize handling, shader registration and extensive GUI/CLI controls.

## Code map

| File | Role |
|---|---|
| `src/shaders/sky/cloud_noise.cs.hlsl` | One-time multiscale noise generation |
| `src/shaders/sky/cloud_shape.cs.hlsl` | Weather, billow envelope and height profile |
| `src/shaders/sky/cloud_model.hlsli` | Shared coordinates/configuration and erosion |
| `src/shaders/sky/cloud_light.cs.hlsl` | Spatial sunlight optical-depth cache |
| `src/shaders/sky/clouds.hlsli` | Advection, intervals, sun visibility and integration |
| `src/shaders/sky/cloud_view.cs.hlsl` | Primary-camera compute integration |
| `src/shaders/sky/sky_lighting.hlsli` | Clear-sky helpers shared with compute |
| `src/shaders/light/dome_light.hlsli` | Primary/secondary sky and solar sampling |
| `src/shaders/path_tracing/path_tracing.rgs.hlsl` | Secondary sample LOD and MIS |
| `src/shaders/light/fog.hlsli` | Fog lighting integration |
| `src/rendering/sky_atmosphere.cpp` | Cloud textures, compute pipelines and refresh |
| `src/rendering/common/common_cloud_settings.h` | Shared 28-float settings layout |
| `src/rendering/renderer/renderer.cpp` | Parameters, resource indices and resizing |
| `src/rendering/renderer/renderer_gui.cpp` | Cloud controls, reset and clipboard export |
| `src/settings_manager.cpp` | Defaults and command-line settings |

See also `knowledge/rendering/clouds.md`. `RtTarget` gained 3D texture support;
RT samplers and heap indices include the cloud resources. Keep CPU/HLSL layouts
and root-constant packing consistent when extending settings.

## User controls

In voxel mode, open **Atmosphere → Cloud settings**. There are four top-level
controls plus 28 advanced parameters:

- Enabled, coverage, density, main march steps.
- **Layer:** base height, thickness, weather tile size, draw distance.
- **Shape:** large/billow frequencies, Perlin/cellular blend, threshold, contrast,
  weather-edge softness, bottom fade, top fade start and top-height variation.
- **Erosion:** medium/fine frequencies and strengths, fine-detail distortion.
- **Lighting:** ambient, forward scattering, multiple scattering, powder, haze.
- **Wind:** X and Z speeds.
- **Quality:** light-cache samples, broad-ray samples, primary resolution divisor.

Ctrl+click sliders for exact values. Higher frequencies mean smaller features.
**Copy cloud settings** copies reusable launch arguments; **Reset cloud defaults**
restores the complete set. Settings changes invalidate accumulation and SHaRC
and refresh cloud fields. Resolution changes queue a GPU-safe renderer resize.

Key defaults: coverage 0.3, density 0.025/m, main steps 32, light steps 12,
broad-ray steps 8, primary divisor 2. Divisor 1 gives full internal resolution.
Coverage is a weather threshold, not guaranteed projected image coverage.
Frequency controls are integers to preserve periodic wrapping. Increasing
coverage, thickness, density or quality can change costs substantially.

## Validation and performance so far

RelWithDebInfo and all shader variants built successfully. Runtime captures
exercised the default settings, full-resolution primary option, and a changed
shape/erosion/layer/wind combination. This verifies basic rendering and parameter
plumbing; it does not establish final visual quality or every live GUI interaction.

RTX 2070, DLSS-RR Balanced, max path depth 4, SHaRC and frame generation off,
unlocked GPU clocks; recent performance runs used animation and 200 measured frames:

| Configuration | Median total GPU frame time |
|---|---:|
| Original procedural/nested-march clouds, 960×540 output | 49.72 ms |
| Original clouds-off comparison, 960×540 output | 8.44 ms |
| Latest defaults, animated, 960×540 output | 9.35 ms |
| Previous full-primary/full-secondary version, 1920×1080 output | 34.05 ms |
| Latest defaults, animated, 1920×1080 output | 31.71 ms |
| Latest clouds-off comparison, 1920×1080 output | 28.95 ms |

The latest 1080p test used 1114×612 internal resolution: **2.77 ms added cloud
cost**. Cloud compute including primary view and light refresh measured 0.334 ms;
the nested `cloud view` scope was 0.165 ms (do not add it to its parent again).
Remaining cost is mainly in path tracing. These are scene-specific measurements,
not a claim about 4070 performance or an accepted final budget.

Local reports/captures are under `build/cloud-controls-final/` and
`build/cloud-parameter-check/`; earlier iterations used `build/cloud-preview/`,
`build/cloud-v2/`, `build/cloud-final/`, and `build/cloud-scales/`.
**These build artifacts and helper scripts are not committed and will not arrive
with the branch on the other PC.** The scene was a copy of the tracked
`tests/voxel/water_reflection` world with camera `phi` changed to 0.22 radians.
Existing golden images were not changed.

Performance reports completed with `timedOut=false`. The app stalled during
test teardown after saving outputs; the supervisor terminated only its own
process after verifying the output. Do not confuse that with a successful
normal shutdown. No fix for this teardown problem was made.

Build normally with `cmake -S . -B build` then
`cmake --build build --config RelWithDebInfo --target Biomeinator`.
New shader files require reconfiguration. See `knowledge/build/configs.md` for
the Windows duplicate-environment workaround if normal building fails; fresh
machines need dependencies built normally, not the skip-dependencies fallback.

## Current limitations and possible next work

- **Shape is still a work in progress.** Defaults can look soft; primary
  upscaling, limited volume resolution, noise/erosion settings and DLSS all merit
  inspection. Let the user tune and share copied settings before choosing a style.
- No dedicated cloud motion/opacity DLSS guides. Static accumulated captures
  do not validate wind/camera-motion ghosting or temporal detail retention.
- Flat, periodic layer with only 32 vertical cache slices. Camera and terrain
  are intended below it. Clouds are applied to environment misses, not finite
  segments to geometry within/above the layer.
- Light storage and multiple scattering are approximate. Broad-ray LOD lowers
  sample count; further filtering/variance work may be useful. No shared
  directional environment cache was added.
- For **towering cumulonimbus**, simply raising thickness makes taller clouds
  but stretches the existing vertical detail. Convincing towers would require
  localized vertical density profiles, columns and anvil shaping, plus adjusted
  cache resolution and marching. Wind/shadows/reflection integration can carry
  over. This is a possible extension, not completed work.

Suggested continuation after the user's own 4070 review:

1. Collect their preferred UI settings and specific visual complaints.
2. Compare primary divisor 1/2, main/broad sample counts, and animated wind/camera
   motion with DLSS-RR; include sharp water reflections and moving ground shadows.
3. Establish a matching clouds-off/on baseline on the 4070 with scene, internal
   resolution, SHaRC and frame-generation settings recorded.
4. Profile secondary-ray cost before further optimization; retain world-space
   coherence and the user's restriction on shared directional environment caching.
5. Decide with the user whether to improve the current layer or extend its shape
   model for cumulonimbus.
