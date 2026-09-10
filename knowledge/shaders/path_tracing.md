_Last edited: 2026-09-09_

# Path Tracing Shader

`src/shaders/path_tracing/path_tracing.rgs.hlsl` — the main rendering shader. `RayGeneration()` computes the pixel coordinate, then `pathTraceRay()` drives the path loop.

This shader does NOT trace primary rays. It reads the G-buffer to get the primary hit, then traces secondary rays from there. See [render_passes.md](../rendering/render_passes.md) for the full pass sequence.

---

## Shared Code

`common/path_tracing_common.hlsli` is shared by this shader, the G-buffer shader, and the light sampling code. It declares the TLAS, vertex/index/instance/per-tri buffers, and provides:

- `getPixelIdx()` / `getPathSplitIdx()` — when path splitting is enabled, the dispatch width is doubled, and even/odd X dispatch indices map to split index 0/1 for the same pixel.
- `AnyHit` — handles two things: (1) alpha cutout via stochastic alpha testing, and (2) refraction passthrough, where transmissive delta surfaces are skipped via `IgnoreHit()` while accumulating path weight and tracking water entry/exit T values.
- `ClosestHit_Primary` — standard hit recording: world-space position, normal (oriented to face the ray origin — i.e. flipped on backfaces), UV, instance/triangle IDs, material index.

---

## Path Tracing Loop

`pathTraceRay()` implements the full path tracing loop. Here is the high-level flow:

### Initialization

The G-buffer provides the first hit. Before the loop starts:
1. Compute **segment absorption** between the camera and the primary hit (handles underwater camera).
2. If the primary ray missed, return the **dome light** color (voxel mode only; returns black in glTF mode).

### Per-Bounce

Each iteration of the loop represents one bounce, up to `effectiveMaxPathDepth`:

1. **Emission** — if the surface emits light, add its contribution weighted by `pathWeight`. On the first bounce, only `pathSplitIdx == 0` handles emission to avoid double-counting when path splitting. At later depths the emission is MIS-weighted against the previous real vertex's light sampling pdf (see step 9).

2. **Path splitting** (first bounce only) — `trySplitMaterial()` deterministically splits the material into two lobes across the two path split indices. Two kinds of splits:
   - **Alpha transparency**: split 0 = opaque (weighted by alpha), split 1 = transparent passthrough (weighted by 1-alpha).
   - **Fresnel**: split 0 = diffuse/transmission + emission (weighted by 1-F), split 1 = specular reflection (weighted by F). Only applied at roughness 0 for now (#372); rough glass in particular can't be split this way, since its Fresnel is per microfacet.
   If the material can't be split, split index 1 early-returns.

3. **Passthrough check** — after `refractionIndirectPassthrough` is enabled and a non-delta surface has been encountered, delta transmissive surfaces (rough glass is a real bounce) are treated as passthrough: the path weight is tinted by the base color, but the position/normal from the previous "real" bounce are preserved. The motivation is noise reduction when sampling lights through surfaces like water — shadow rays use the same passthrough logic, so indirect paths and direct estimates stay consistent. This sacrifices some accuracy but is worth it for real-time.

4. **SER reordering** — `NvReorderThread()` sorts threads by a coherence hint (first bounce, passthrough, or scattering non-delta surface) to improve warp occupancy.

5. **Russian roulette** — from depth 2 onward, paths may be terminated probabilistically based on luminance of the throughput, with a minimum 10% survival probability. Never applied in ReSTIR replay mode (see [restir → design.md](../restir/design.md)).

6. **BSDF sampling** — `sampleBsdf()` picks a direction (lobe model in [materials.md](materials.md)):
   - **Glossy reflection**: mirror `reflect()` when roughness is 0, otherwise a GGX VNDF half vector; weighted by `glossyReflectionTint` and the macro-normal Fresnel.
   - **Dielectric (reflection + transmission)**: one VNDF half vector (the normal when roughness is 0), with the per-microfacet Fresnel choosing `reflect()` or `refract()` about it; transmission is weighted by base color.
   - **Diffuse**: cosine-weighted hemisphere sampling with Lambertian BRDF, accounting for Fresnel reflection loss.

   The Fresnel decision is stochastic — a random number is compared against the Walter Fresnel reflectance. Rough samples finish through the shared `evaluateBsdf` (value and pdf together) so the estimator agrees with NEE's MIS terms.

7. **Direct light sampling** (MIS/RTSL only, non-delta surfaces) — two independent strategies, both MIS-weighted against the BSDF sample using the balance heuristic:

   - **Area lights**: in MIS mode, one light is picked uniformly and a shadow ray is traced. In RTSL mode, the light tree picks a light instead. The MIS weight uses the solid-angle pdf of the chosen light.

   - **Dome light** (voxel mode only): a direction is sampled uniformly within the sun's spherical cap and a shadow ray is traced. If it misses all geometry, the dome light radiance (sun or sky gradient) is added. This is separate from area light sampling because the two can't produce each other's samples (dome light can't hit area lights and vice versa), so their MIS weights are independent.

8. **Trace next ray** — `TraceRay` from the BSDF-sampled direction. Update material, ray cone width, segment absorption.

9. **BSDF-hit emission MIS** — if the BSDF-sampled ray hit an emissive surface, its emission is MIS-weighted against the light sampling pdf (only for non-specular bounces, since specular has zero light sampling probability). Dome light pdf is also factored in if the ray missed (dome light hit via BSDF sampling). Like the NEE and dome-light cases, the weight is applied to the emission contribution only, never to `pathWeight`: a path that continues past the emissive vertex can only have been produced by BSDF sampling (NEE terminates at the light), so its continuation must keep full throughput. This only matters for a surface that both emits and scatters (glTF materials may; voxel emissive texels have zero diffuse and never do).

The loop also serves ReSTIR PT: every complete path is a candidate for the pixel's reservoir, and the same loop replays a stored path from its seed. RNG draws are per-vertex, per-purpose streams for that reason. See [restir → design.md](../restir/design.md).

### Albedo Guide Outputs

Alongside `pathColor`, the shader computes `ptDiffuseAlbedo` — a denoiser input for first-bounce diffuse albedo. When the first bounce is specular with path splitting enabled, it looks through to the second hit's base color plus Reinhard-compressed emission, modulated by the first bounce's specular tint. It also implicitly captures volume absorption and other effects accumulated in `pathWeight` up to that point.

A **rough glossy** first bounce takes a different route (`computeFirstBounceAlbedos`), because its lobe is picked stochastically per sample and the resulting path weight is pure noise as a guide: the two lobes are weighted analytically by the macro-normal Fresnel instead, and the shader writes the specular albedo target itself, overwriting what the G-buffer pass put there. Glass puts both of its lobes in the specular guide and leaves the diffuse guide black, since the diffuse guide has no lobe to represent refraction and demodulating it there would smear the refracted image; everything else splits diffuse from glossy reflection. Rough glass's real sampling weights its lobes per microfacet, so this is deliberately an approximation — a guide buffer only has to track the signal's magnitude. Notes:

- The weight folded in is the path weight *before* scattering, so both guides carry the same absorption, fog transmittance and alpha-split weight as the radiance they demodulate.
- Only `pathSplitIdx == 0` can reach a rough material — `trySplitMaterial` breaks split 1 out of anything it can't split, and the alpha split leaves split 1 a delta passthrough — so the single write to the shared specular albedo target has no second writer to race with.
- Non-glossy materials keep the path weight as their guide: it is already noise-free for a lone diffuse lobe, and it is what carries the diffuse-transmission and passthrough cases.
- The DLSS-RR integration guide names this case: §3.5 says a noisy guide confuses RR into grainy or ghosting output and that, where the noise can't be avoided *"such as with ray traced glossy refraction"*, constant values for the refracting surface's albedo, normal and roughness let RR converge to a smooth result. §3.4.1 leaves the blend formula to the application but requires that it not be noisy. It gives no rule for where refraction belongs — §3.4.2 defines specular albedo as reflectivity — but the diffuse guide is the worse home for it, since a sharp view-dependent refracted image demodulated as diffuse is what invites RR to reconstruct background detail through the glass.
- `calculateDlssSpecularAlbedo` expects an F0 specular color (it ramps Fresnel over view angle internally), while `glossyReflectionTint` is a Cycles-style lobe tint whose dielectric Fresnel is applied outside the lobe. Multiplying the fit by the macro Fresnel is what reconciles the two conventions. **Roughness 0 still passes the tint unweighted** from the G-buffer pass, so the delta case reports a higher specular albedo than the guide's convention would.

Emission stands in for albedo in this guide (a bright emitter must not read as a black surface), compressed with Reinhard so it stays in range. A surface can both emit and scatter, so the primary hit's emission is kept in a separate `ptEmissiveAlbedo` and summed (saturated) into the guide only after the loop: the specular look-through above scales and zeroes the scattered part, and must not touch the emission part. Pure emitters and pure scatterers get exactly one of the two terms, so this is a no-op for them; the `diffuse_albedo_modulation_*` and `diffuse_and_emission_diffuse_albedo` goldens pin all three cases.

---

## Voxel-Mode-Only Features

The following are gated on `sceneParams.voxelMode == 1` and return zero/noop in glTF mode:

- **Dome light** — `getDomeLightColor()` returns black in non-voxel mode. This means all environment lighting (sun, sky gradient, ground color) is voxel-only.
- **Dome light direct sampling** — the entire dome light MIS branch is skipped in non-voxel mode.
- **`domeLightPdf()`** — returns 0 in non-voxel mode, so BSDF-hit MIS against the dome light has no effect.
- **Chunk debug coloring** — tints the primary hit based on chunk coordinates.
- **Water absorption** — `computeSegmentAbsorption` and `computePassthroughAbsorption` use `voxelBoundsMin/Max_WS` to compute water travel distance, which is only populated in voxel mode. The `PerTriData` flag `TRIANGLE_FLAG_IS_WATER` is also never set outside voxel mode, so water-surface detection itself is voxel-only.
- **Orphan water backface termination** — hitting a water backface without having crossed a front face or started underwater means the water volume is open (front-face chunk not loaded yet). Paths are terminated at such hits (`isOrphanWaterBackfaceHit`), after the segment's fog/absorption but before any surface interaction: continuing would trace the water interior flagged as air — bounce-segment fog in-scatter below sea level plus unattenuated dome light through missing chunks — which glows and flickers as chunks stream in. Cannot trigger on sealed geometry.
- **Air fog / god rays** — per-segment height fog with single-scattered sunlight, applied at the two segment-absorption call sites (skipped for underwater segments). Transmittance is closed-form; in-scattering marches jittered steps with one sun occlusion ray each (`light/fog.hlsli`) — an inline `RayQuery` that alpha-tests cutout candidates (deterministic 0.5 threshold, mip 0 — occlusion is boolean, so no ray cone or stochastic alpha) and does not occlude on glossy-transmissive materials (water lets sun shafts through) — plus an analytic `fogAmbientStrength * (1 - T) * avgSky` ambient term with no visibility check — it also brightens enclosed cave interiors, and the strength setting is the artistic control for that trade-off. Non-obvious rules:
  - Primary-segment in-scatter is gated on `pathSplitIdx == 0` (like emission); bounce segments are ungated, which is what puts god rays into the reflection split.
  - In-scattering (sun march + ambient) runs only at path depths ≤ 1 (half steps on bounce segments); deeper bounces keep transmittance only.
  - `renderParams.fogSigmaS` is computed per frame on the CPU (`computeFogSigmaS` in renderer.cpp): a fixed peak coefficient times a time-of-day smoothstep ramp peaking at sunrise/sunset times the `fogScatteringMultiplier` setting. `fogSigmaS = 0` (mid-day, or multiplier 0) must skip the entire fog block (including the march's RNG draws) so renders stay bit-identical with fog off.
  - Sea level comes from `SEA_LEVEL` in `common_settings.h` (shared with `chunk_generator.cpp`); fog height math adds `globalInstanceOffset.y` to get true world Y.

---

## Hit Groups

Two hit groups exist in the path tracing pipeline:

- **`HITGROUP_PRIMARY`** — used for BSDF-sampled rays. Full closest-hit that records the complete hit, plus anyhit for alpha cutout and passthrough.
- **`HITGROUP_LIGHTS`** — anyhit-only (same anyhit as primary, for passthrough); a triangle
  hit group needs no closest hit shader. Initial sampling's shadow rays trace inline, so only
  the ReSTIR reuse passes' reconnection and NEE replay rays go through it (`TraceRay` via
  `isSegmentOccluded` with `useRayQuery` false).

Shadow rays (area-light NEE in `traceToLight`, dome light in `traceToDomeLight`) go through
`isSegmentOccluded`, an inline `RayQuery` in the raygen shader rather than a `TraceRay` (in
initial sampling; the reuse passes pass `useRayQuery` false, see below). The
anyhit body lives in `acceptHitCandidate`, which the `AnyHit` entry point and the query's
`Proceed` loop both call, so a segment applies the same passthrough tint, water entry/exit
tracking and stochastic cutout whichever way it is traced; the `Payload` at those call sites is
a plain local carrying that state. The inline query skips the payload marshalling and
shader-table dispatch a `TraceRay` pays per non-opaque candidate: measured against the DXR
occlusion-ray idiom (`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
RAY_FLAG_SKIP_CLOSEST_HIT_SHADER`, itself worth ~9% frame time in 2026-08) it took -8% path
tracing on cornell_box/evil_room and -11..-16% on cave_lights/fog_god_rays (2026-09, A/B/A,
goldens unchanged to 1e-5). Inline traversal gives up SER and adds live state to the caller, so
it is only a win for rays with no closest hit; primary and bounce rays stay `TraceRay`. In
the ReSTIR reuse passes even the shadow rays stay `TraceRay`: those kernels are SER-sorted by
reconnection type and register-bound, and inline queries there measured +10..16% on
cave_lights/evil_room (2026-09). Any
committed hit means occluded. Consequences:

- Area-light shadow rays no longer verify they hit the sampled light triangle; instead
  `traceToLight` stops `TMax` just short of the light and treats any committed hit as
  occlusion. Le is evaluated analytically from the sampled point's barycentrics
  (`lightBary2`, threaded from `sampleAreaLightPoint` through both sampling modes) —
  needed because with no closest hit there is no interpolated UV, and emissive textures
  (packed-aux lamps/lava) still require one.
- **TMax gotcha:** the shadow-ray origin is offset along the surface normal, which shifts
  the ray parallel to itself — it crosses the light's *plane* earlier than the
  surface-to-sample distance at oblique angles (shortfall `eps·dot(m,n)/dot(m,wi)`,
  unbounded at grazing). `TMax` is therefore computed from the offset ray's light-plane
  intersection, not from the sample distance; using the distance self-shadows the light at
  grazing angles (caught by the `occluded_light` golden).
