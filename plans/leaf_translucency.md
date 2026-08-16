# Leaf Translucency (Thin Diffuse Transmission)

Goal: make leaf blocks look soft and backlit instead of harshly shadowed — sun behind a
leaf should illuminate its front, and canopy interiors should glow. This is Blender's
Translucent BSDF: Lambertian **diffuse transmission** through a thin surface, mixed with
the existing diffuse reflection. Investigated 2026-08-16 against the current codebase.

## Model

A thin-wall translucent surface with translucency factor τ splits the diffuse albedo
between the two hemispheres:

- reflection (viewer side): `albedo * (1 - τ) / π`
- transmission (far side): `albedo * τ / π`

Reflect + transmit sums to the albedo, so it is energy conserving. τ ≈ 0.3 is a good
starting point (tune 0.25–0.4). τ = 0 must reproduce current output exactly — that is
both the correctness check and the golden-test escape hatch.

Note that `ClosestHit_Primary` already flips the hit normal to face the ray origin
(`path_tracing_common.hlsli:194`), so "front hemisphere" always means the viewer side and
the thin-wall model comes out symmetric for free — no backface special case in the BSDF.

## Why a per-triangle flag, not a new material

Leaves are not their own material: the whole terrain instance uses
`TerrainMaterial::DEFAULT` (`chunk.cpp:762`), and materials are per-instance. A dedicated
leaves material would require a third instance per chunk (like water), touching instance
management and the TLAS for no benefit. `PerTriangleData.flags` already solves exactly
this problem for biome tint (`TRIANGLE_FLAG_BIOME_TINT`), and the path tracer already
fetches `perTriData` at the top of every bounce (`path_tracing.rgs.hlsl:183`), so the
flag can be promoted onto the local `surfMaterial` copy with no payload changes.

## Facts verified against the code (what makes this small)

- The bounce-ray origin offset already faceforwards: `path_tracing.rgs.hlsl:466` passes
  `true /*faceforwardNormal*/`, so BSDF-sampled transmission rays into the back
  hemisphere already get a correctly offset origin.
- Both direct-lighting contribution terms already use `absCosTheta` for the surface
  cosine (`path_tracing.rgs.hlsl:380` for area lights, `:425` for the dome light), as
  does the path-weight update (`:453`). No geometry-term changes needed.
- `domeLightPdf` (`dome_light.hlsli:83`) has no hemisphere gating, so MIS between BSDF
  sampling and dome-light sampling stays consistent once backside samples are allowed.
- BSDF-hit dome light MIS (the miss path) works for transmitted directions automatically.
- All hemisphere gating lives in exactly three places: `evaluateBsdf`, `bsdfPdf`
  (`materials.hlsli:158` / `:258`, the `dot(wi_WS, surfNor_WS) < 0` early-outs), and the
  dome light sample rejection (`dome_light.hlsli:129`).
- The DEFAULT terrain material has no glossy reflection, so the Fresnel terms in the
  diffuse branch are inert for leaves (fresnelReflectance = 0).

## Implementation

### 1. Data plumbing (CPU)

- `common_structs.h`: add `TRIANGLE_FLAG_THIN_TRANSLUCENT (1 << 3)`. Repurpose
  `Material.pad2` as `float translucency` (default 0 in the `Material()` ctor). No new
  material flag bit: treat `hasDiffuse() && translucency > 0` as the condition (add a
  `hasThinTranslucency()` helper). This keeps τ = 0 structurally identical to today.
- `block.h` / `block.cpp`: add `bool translucent{ false }` to `BlockData`; set it on all
  `*_LEAVES` entries. Keying off `BlockType::TRANSPARENT_CUTOUT` instead would also catch
  non-leaf cutout blocks — the explicit field is cleaner.
- `chunk.cpp` cube-face meshing (~line 727, where `TRIANGLE_FLAG_BIOME_TINT` is set): OR
  in the new flag when `blockData.translucent`. The X-shaped branch (~line 673) can get
  the same treatment later if grass tufts / flowers should be translucent too — separate
  decision, skip for now.
- Setting: `ADD_OPTION("leafTranslucency", ..., float, "0.3")` in `settings_manager.cpp`
  (+ `COPY_SETTING`), copy into a new `RenderParams` field next to the fog params
  (`renderer.cpp:641`), GUI slider in `renderer_gui.cpp` (~line 129) ORed into
  `renderState.didPathTracingSettingsChange` so accumulation resets on change.

### 2. Promotion at shading time (path_tracing.rgs.hlsl)

At the top of the bounce loop, right after `perTriData` is fetched (line 183):

```hlsl
if (bool(perTriData.flags & TRIANGLE_FLAG_THIN_TRANSLUCENT))
{
    surfMaterial.translucency = renderParams.leafTranslucency;
}
```

`surfMaterial` is re-fetched fresh from `materials[]` after every `TraceRay`
(`path_tracing.rgs.hlsl:482`), so per-hit promotion at the loop top is correct across
bounces with no cleanup.

### 3. The BSDF (materials.hlsli)

- `evaluateBsdf` (line 158): instead of returning 0 for `dot(wi_WS, surfNor_WS) < 0`,
  return the transmission lobe `diffuseAlbedo * M_INV_PI * material.translucency` (front
  side becomes `* (1 - material.translucency)`, keeping the existing `(1 - F)` factor).
  Back side needs no Fresnel term.
- `sampleBsdf` diffuse branch (line 243): draw `rng.nextFloat()`; with probability τ,
  cosine-sample around `-surfNor_WS` instead of `+surfNor_WS`. Scale the pdf by τ
  (back) / `1 - τ` (front). Both lobes then have `bsdf * cos / pdf = albedo` — perfect
  importance sampling either way, so path weights stay noise-free.
- `bsdfPdf` (line 258): mirror the hemisphere split — back side returns
  `hemisphereCosineWeightedPdf(wi_WS, -surfNor_WS) * τ`, front side keeps the current
  value times `1 - τ`. MIS breaks silently if this doesn't match `sampleBsdf` exactly.

### 4. Direct light sampling

- `light_sampling.hlsli:102` (`traceToLight`): change `setRayOriginAndDirection(...,
  false)` to faceforward the offset normal. Safe unconditionally: for opaque surfaces a
  backside light direction already contributes zero via `evaluateBsdf`, and the
  TMax-from-light-plane logic just below (`:105`–`:113`) uses the post-offset
  `ray.Origin`, so it stays consistent.
- `dome_light.hlsli` `sampleDomeLight`: the backside rejection at line 129 must be
  skipped for translucent hits (pass a `bool isTranslucent` parameter — don't drop the
  rejection unconditionally, it saves a shadow ray on every opaque surface with the sun
  below the horizon of the shading point). Faceforward the offset at line 136 the same
  way. This is where most of the visual payoff comes from: the sun-cap sample now lights
  leaves from behind.

## Interactions checked — no changes needed

- **Alpha cutout / AnyHit**: untouched. Cutout holes already leak light stochastically;
  translucency adds transmission through the *opaque* leaf texels, which is what softens
  the harsh shadows. Per the OMM investigation (`plans/opacity_micromaps.md`), terrain
  alpha is strictly binary, so the two mechanisms compose without overlap.
- **`trySplitMaterial`**: the alpha split keeps flags and the translucency field on
  split 0; split 1 replaces the material wholesale with a passthrough (no diffuse →
  `hasThinTranslucency()` false). The Fresnel split can't trigger for leaves (no glossy
  reflection). No changes.
- **NRC**: `nrcSurfAttr` needs no change — roughness/delta classification is unaffected,
  and the cache is trained on actual path radiance, so it learns the transmitted light.
- **`ptDiffuseAlbedo`**: computed from `pathWeight` after the BSDF weight is applied
  (`path_tracing.rgs.hlsl:463`), so it picks up the τ split automatically.
- **G-buffer pass**: outputs geometry and first-hit albedo only; first-order guide
  buffers for DLSS are unchanged by a new lobe.

## Known limitation (defer)

Light-tree/RIS importance uses `geomTermBound` (`light_tree_sampling.hlsli:70`), which
returns 0 when a light's bbox is entirely behind the surface plane — so area lights
(lava, lamps) *behind* a leaf won't be RIS-sampled through it, even though the BSDF now
supports it. BSDF-sampled paths still find them, just noisily. Sun/sky dominates the
canopy look, so this is fine to defer; the fix if wanted later is
`max(geomTermBound(N), geomTermBound(-N))` for translucent hits, which needs the
translucency bit threaded into the RIS target function.

## Testing

- τ = 0 must be bit-identical to current output (same reasoning as `fogSigmaS = 0`
  skipping the fog block — verify no extra RNG draws on the τ = 0 path; the sampling
  change draws from `rng` only inside the diffuse branch it already draws in, so
  structure the code to keep the draw count identical).
- Goldens: every scene with trees changes when the default is nonzero. Suggested order:
  land the feature with default 0 → tune τ visually in the GUI → flip the default and
  update goldens in one commit, adding a dedicated `leaf_translucency` golden while at it.
- Visual checks: sun low behind a tree (leaves should glow, not silhouette); under a
  dense canopy at noon (soft dappled light, not black); night + lava under a tree
  (backside area lights will be noisy — expected, see limitation above).
