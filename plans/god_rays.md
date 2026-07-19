# God Rays (Volumetric Single Scattering)

Height-based fog with single scattering (sun shafts), applied per path segment so it
shows up in reflections and refractions automatically. Transmittance is closed-form;
in-scattering marches a few jittered steps with one sun occlusion ray per step.
Voxel mode only (sun/dome light is voxel-only).

## Fog density profile

Piecewise in true world-space Y (see gotcha about `globalInstanceOffset` below):

- Below `seaLevel - fogRampBlocks`: zero density — deep caves stay fogless.
- Ramp zone (`seaLevel - fogRampBlocks` → `seaLevel`): linear ramp from 0 to `fogSigmaS`.
  Linear (not smoothstep) so the optical-depth integral stays a simple quadratic; the
  ramp is mostly underground so the visual difference doesn't matter.
- Above `seaLevel`: exponential falloff `fogSigmaS * exp(-(y - seaLevel) / fogScaleHeight)`.
  Standard atmosphere-style profile, integrates in closed form.

`fogRampBlocks = 48` initially (per design intent); tune later if cave openings look
wrong. `seaLevel` (125) currently lives in `chunk_generator.cpp` — needs to reach the
shader via scene params rather than being duplicated.

The optical depth of a segment is the sum of closed-form integrals over the (up to
three) zones the segment crosses, split at the two boundary heights. The exponential
zone needs the numerically stable near-horizontal branch (limit as Δy → 0).

## Stage 1: air fog

### Parameters

New settings: `fogSigmaS` (peak scattering coefficient), `fogScaleHeight`, `fogG`
(Henyey-Greenstein anisotropy), march step count. These change accumulated radiance,
so they must set `didPathTracingSettingsChange`. `fogRampBlocks` can stay a shader
constant.

### Transmittance

New closed-form function in `volume.hlsli` next to `computeWaterAbsorption`.
Multiplied into `pathWeight` at the two existing segment-absorption call sites in
`path_tracing.rgs.hlsl` (primary segment from the G-buffer hit, and after each bounce
`TraceRay`). No G-buffer shader change. On miss, clamp the integration distance
(voxel-bounds exit like `getDistanceToVoxelBounds`, or a fixed cap) — horizontal rays
otherwise have unbounded range.

### In-scattering

Per segment, N jittered steps (`payload.rng`); ~4–8 on the primary segment, fewer (or
restrict marching to path depths 0–1) on deeper bounces. Each step:

- camera-side transmittance to the step point (closed form),
- density at the step height × HG phase (sun dir · ray dir),
- sun transmittance above the step point (closed form along `sunDir_WS`, converges
  since the sun ray goes up),
- sun visibility via one occlusion ray,
- × `sunColor`, accumulated and added as `pathColor += pathWeight * inScatter` at the
  same call sites as transmittance.

No MIS partner needed: the sun is effectively a delta directional light, and rays that
miss into the sun already get direct transmission via the dome light path, so there is
no double counting.

Also add a cheap analytic ambient term — `(1 - segment transmittance) × average sky
color`, no shadow rays — so distant terrain reads hazy instead of just dark
(aerial perspective).

### Occlusion rays

`TraceRay` with `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE |
RAY_FLAG_SKIP_CLOSEST_HIT_SHADER` against the existing TLAS. No new hit group or
shader-table change: only the miss shader can run, and the existing `Miss` clears
`PAYLOAD_FLAG_DID_HIT`, so initialize the payload with the flag set and check it after
the trace. Must use the full `Payload` struct (miss shader declares it); initialize
minimally. `FORCE_OPAQUE` means alpha-cutout foliage and water surfaces occlude fully —
acceptable for stage 1.

### Path splitting / denoiser routing

The primary segment is identical for both splits and collect sums them, so gate
primary-segment in-scatter (and the ambient term) on `pathSplitIdx == 0`, same as
emission and the dome-light miss. Per-bounce segments diverge after the split — no
gating, and this is what puts god rays into the reflection split.

Fog radiance flows through the normal `pathColor` → raw buffer → collect → DLSS-RR
path; step/shadow-ray noise is denoised as radiance. Watch for DLSS-RR smearing or
ghosting the shafts (view-dependent volumetric with an albedo-independent
contribution); use the debug-texture technique if it misbehaves.

### NRC

Fog transmittance in `pathWeight` is picked up automatically by `prefixThroughput`.
In-scatter is only accumulated in `pathColor` up to the NRC query/termination vertex —
stage 1 accepts a slightly fogless NRC tail rather than teaching the cache about fog.

TODO: check visually how NRC-enabled rendering looks with fog vs. plain path tracing;
if the fogless tail is noticeable, revisit marching in the NRC update pass so the
cache learns in-scatter.

### Gotchas

- Include order: `volume.hlsli` is included before `dome_light.hlsli`, so fog code
  there can't see `sunDir_WS`/`sunColor`. Hoist the sun constants into a small shared
  header, or put the march in a file included after the dome light.
- Shader world space is offset by `globalInstanceOffset`; fog height math must add
  `cameraParams.globalInstanceOffset.y` to get true world Y before comparing against
  sea level.

### Verification

Golden-image sanity via the usual voxel goldens (fog off should be bit-identical).
Visual checks: shafts through tree canopies / cave openings, shafts visible in water
reflections, deep caves fog-free, accumulation-mode convergence with DLSS off.

## Stage 2: underwater scattering

Uniform medium (no height profile): reuse `waterSigmaA`, add a scattering coefficient
and likely a different HG `g`. Transmittance is the existing plain exponential.

The hard part is sun occlusion: rays from underwater points must pass through the
water surface, so the cheap `FORCE_OPAQUE` occlusion ray can't be used. Instead trace
dome-light-style rays (`PT_HITGROUP_DOME_LIGHT` with
`PAYLOAD_FLAG_REFRACTION_PASSTHROUGH`) — anyhit runs, the surface is passed through in
a straight line (consistent with existing dome sampling from underwater surfaces,
which already ignores refraction bending), and `waterEntryT`/`waterExitT` +
`computePassthroughAbsorption` give the water-path absorption. Noticeably more
expensive per step than the air version.

Medium selection per segment via `PAYLOAD_FLAG_UNDERWATER`. Passthrough segments that
straddle the surface inherit the existing single-entry/exit limitation of
`computePassthroughAbsorption`; accept that.
