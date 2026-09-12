# World-space clouds

**Current implementation:** the shape now follows the user's Blender graph,
with 2D Smooth F1, 2D noise distortion, 3D noise detail, and a hard flat base.
See `blender/clouds/README.md` for current controls/defaults and validation.
The implementation, tuning, and performance sections below describe the
superseded model and are retained as historical context.

Clouds occupy an absolute-world altitude layer, default 3000–4500 m. Wind advects
XZ density using animation time; camera movement and origin rebasing do not
move the clouds. Ground shadows, solar visibility, reflections and fog sun
lighting use the same spatial density/light model. No shared directional
environment cache is used.

## Implementation

- `cloud_noise.cs.hlsl` generates periodic 64³ RGBA16F noise once, packing
  three-octave gradient Perlin fBM and three cellular scales.
- `cloud_shape.cs.hlsl` builds a 128×32×128 R16F envelope on settings changes.
  Domain-warped weather selects masses; Perlin/cellular billows and tapered
  column summits provide structure. A shared rotated basis avoids cardinal alignment.
- `cloud_model.hlsli` adds medium erosion and domain-warped fine erosion using
  texture fetches. Erosion only removes density, allowing empty-envelope skips.
- `cloud_light.cs.hlsl` builds spatial optical depth toward the sun, refreshing
  on animation time or settings changes. Runtime sunlight is a volume lookup
  instead of a nested density march.
- `clouds.hlsli` integrates radiance/transmittance with frame-stable jitter and early
  opacity termination. Travel inside the layer is bounded by six thicknesses or
  12 km, whichever is smaller. Multiple scattering and aerial perspective are approximate.
- `cloud_view.cs.hlsl` integrates primary clouds at half internal resolution by
  default. Sky misses bilinearly sample radiance/transmittance; the solar disk
  remains full resolution. This camera buffer is not used for surface lighting.
- Secondary misses march from actual origins. Ray-cone angle selects full
  samples for narrow rays and fewer samples for broad rays. Sun NEE evaluates
  solar transmittance; escape-ray MIS separates sun and cloud scattering.

SkyAtmosphere owns resources and compute pipelines. Shared CloudSettings has
matching CPU/HLSL layout. UI changes reset accumulation and SHaRC and refresh
fields; changing the resolution divisor queues a resize after GPU flush.
Integer frequencies maintain the periodic seams.

## Tuning

Open **Atmosphere → Cloud settings** in voxel mode. Defaults: coverage 50%,
density 0.015/m, march steps 128, period 16384 m. Coverage adjusts a weather
threshold; it does not guarantee exactly 50% projected cloud coverage.

| Group | Controls |
|---|---|
| Layer | Base height, thickness, tile size, draw distance |
| Shape | Large/billow frequencies, Perlin/cellular blend, threshold, contrast, weather softness, bottom/top profiles |
| Erosion | Medium/fine frequencies and strengths, fine-detail distortion |
| Lighting | Ambient, forward scattering, multiple scattering, powder, distance haze |
| Wind | X and Z speed in m/s |
| Quality | Light-cache samples, broad-ray samples, primary resolution divisor |

Higher frequencies produce smaller features. Start with Shape, then medium
and fine erosion. Ctrl+click for exact slider entry. **Reset cloud defaults**
restores the complete set; **Copy cloud settings** copies reusable launch
arguments. Primary divisor 1 is full internal resolution; 2 is the default.
Increasing coverage, density, distance or samples can increase frame cost.

## Validation (2026-09-12)

The measurements below predate the PC follow-up and apply to the old defaults.
See `plans/clouds.md` for the follow-up build/capture checks and limitations.
Fog now uses four terrain shadow directions and fractional solar-disk visibility;
the cloud light cache is continuous through sun elevation zero. The performance
impact of these changes has not yet been measured.

RelWithDebInfo and all shader variants compiled. Runtime captures and 200-frame
GPU measurements use RTX 2070, DLSS-RR Balanced, max path depth 4, SHaRC and
frame generation disabled. Animation runs during performance measurement.
Clocks are unlocked; results apply to this scene and default cloud settings.

At 1920×1080 output (1114×612 internal), median total GPU frame time is
31.71 ms with clouds versus 28.95 ms without: **2.77 ms added**. The previous
full-primary/full-secondary march version was 34.05 ms in this scene. Cloud
compute including view and refreshed lighting is 0.334 ms; remaining cost is
mainly path tracing. Its nested cloud-view scope is 0.165 ms: do not add the
nested scope to the parent again.

At 960×540 output the latest animated-cloud median is 9.35 ms; the original
procedural/nested-march prototype was 49.72 ms. Reports and default preview are
in `build/cloud-controls-final/`. Performance reports completed 200 frames with
`timedOut=false`. Test teardown stalls, so the supervisor terminates its owned
process after output is saved. Existing golden images were not changed.

## Limits

The flat periodic layer assumes camera and terrain below clouds. Compositing
occurs on environment misses; finite segments through clouds to geometry in
or above the layer are not integrated. Low-resolution lighting and limited
march samples are approximations. Fog ambient remains approximate. Dedicated
cloud motion/opacity DLSS guides are not implemented, so accumulated screenshots
do not establish animated reconstruction quality. Primary upscaling can soften
edges; the quality controls expose this tradeoff.
