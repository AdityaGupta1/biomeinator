_Last edited: 2026-09-02_

# Material Model and BSDFs

`src/shaders/materials/materials.hlsli` with the GGX helpers in `src/shaders/util/ggx.hlsli`.
A material is a set of lobes toggled by flags (diffuse, glossy reflection, glossy transmission;
see [common_structs.md](common_structs.md)), one roughness shared by both glossy lobes, and per-lobe
tints (`baseColor` for diffuse and transmission, `glossyReflectionTint` for reflection).

## Two Fresnel conventions

Which lobe a path takes is decided by a dielectric Fresnel weight, but *where* Fresnel is evaluated
differs by lobe combination, deliberately mirroring the Blender node group used for the reference
renders (see [scene → blender_export.md](../scene/blender_export.md)):

- **Glossy reflection over diffuse** (and reflection alone): macro-normal Fresnel.
  `glossyReflectionProbability` evaluates `walterFresnel` with the shading normal and is the single
  source of the value used as lobe-selection probability, lobe weight, and pdf weight. Matches
  Blender's Glossy + Diffuse mixed through a Fresnel node. Physically approximate at high roughness
  (Principled would weight diffuse by the specular lobe's directional albedo instead) but never
  loses energy.
- **Glossy reflection + glossy transmission** (glass, `sampleDielectricBsdf`): per-microfacet
  Fresnel. One VNDF half vector is sampled and `F(h)` picks reflect vs refract about it, so total
  internal reflection at a microfacet just reflects instead of producing a dead sample. Structured
  exactly like Cycles' `bsdf_microfacet_sample`/`eval` for the glass closure; the reference is the
  OSL `dielectric_bsdf` closure with separate reflection/transmission tints.

A transmissive material never has a diffuse lobe (asserted in `Scene::addMaterial`), and a
transmission-only material (used for alpha passthrough) must be perfectly specular. With
`roughness == 0` both conventions coincide, so the delta glass path is unchanged by the rough one.

## Rough refraction details

- Evaluation reconstructs the half vector from `(wo, wi)`: `wo + wi` for reflection, Walter's
  `-(ior * wi + wo)` for refraction. The refraction half vector points to the lower-IOR side, so it
  is flipped onto the shading normal's side; D, G and Fresnel are symmetric in that sign.
- The refraction formula yields *some* half vector for every `wi` below the surface, including ones
  that put `wo` and `wi` on the same side of `h`, which no refraction can produce. Evaluation must
  return zero (value and pdf) for those, not just for `wo·h <= 0`; otherwise NEE credits unreachable
  directions and light-sampled renders come out brighter than BSDF-sampled ones. Cycles' eval has
  the same gap (a TODO in `bsdf_microfacet_eval`), which is why its light-sampled rough glass is too
  bright — see [tests → golden_tests.md](../tests/golden_tests.md) for how the reference avoids it.
- A relative IOR within `DIELECTRIC_PASSTHROUGH_IOR_EPSILON` of 1 is sampled as a delta passthrough
  (as Cycles does): refraction then gives `wi = -wo`, for which the half vector degenerates.
- The refraction Jacobian `ior² |wi·h| / (ior wi·h + wo·h)²` is what Cycles' `sqr(ior * inv_len_H)`
  term is. There is **no η² radiance scaling** anywhere: Cycles applies none, and the delta path
  (throughput = tint) already matches it. Value and pdf share the Jacobian, so BSDF-sampled
  throughput reduces to `G2/G1 · compensation`.
- Multiple-scattering compensation for glass uses Cycles' IOR-indexed tables
  (`ggxGlassETable` etc. in `ggx_tables.hlsli`, `16³` over roughness/cosθ/`z = sqrt((ior-1)/(ior+1))`,
  with the `Inv` pair for relative IOR < 1 looked up with `1/ior`) and the same `1 + Fms(1-E)/E`
  form as reflection, with the transmission tint as Fss. The compensation is large at high
  roughness (more than 2× for the inside interface at roughness 1). Cycles' inverse tables
  disagree with its own sampler — they count refracted samples that surface on the reflection
  side, which the sampler kills — so exiting glass at grazing angles is under-compensated. They
  are kept as-is so the engine and Cycles stay identical in BSDF-sampled transport.
- Samples that end up on the wrong side of the surface for their lobe are killed (value 0, pdf 1)
  and the path continues with zero weight — kill-and-continue is unbiased, and the pdf needs no
  renormalisation because it is the true sampling density. Killed samples must still carry a valid
  direction: an uninitialised or zero direction feeds NaN geometry into the next bounce, and the
  accumulation then loses the whole pixel sample.
- `bsdfPdf` must mirror `sampleBsdf`'s lobe structure exactly (including the diffuse-transmission
  hemisphere split) or MIS silently breaks; `sampleBsdf` finishes non-delta samples through the
  same `evaluateBsdf`/`bsdfPdf` used by NEE for that reason.

## Verifying energy behaviour

A furnace (glass spheres inside a large inward-facing emitter of radiance < 1, plus a white
diffuse control sphere, `--maxPathDepth=64`) must read the emitter's radiance everywhere; it
exposes lobes that credit unreachable directions but is blind to two other things, so use it
together with a real scene:

- It cannot see NEE *magnitude* inconsistencies (the light pdf is negligible against a huge
  emitter) or directional errors (the incident radiance is uniform). Compare `--samplingMode=0`
  (naive) against MIS/RTSL on a scene with a small light for those; they must agree.
- Bounce limits: Cycles' `max_bounces = N` allows N scatter events plus a final emission hit, the
  engine's `--maxPathDepth=N` allows N - 1, and silhouette paths through low-roughness glass are
  long (repeated internal reflection), so at 12 bounces the rims of a roughness-0.25 sphere come
  out ~5% darker than Cycles' even though both lobes are identical. Compare at 64.

## Light sampling interaction

`Material::acceptsBacksideLight()` (diffuse transmission or rough glossy transmission) is the one
predicate that tells light sampling a surface can scatter light arriving from behind the shading
normal: it widens the light-tree bounds, disables the dome-light backside rejection, and is stored
per bounce for the BSDF-hit emission MIS weight. Forward selection and pdf evaluation must use the
same value or the MIS weights disagree. Rough glass is not passthrough: the anyhit shader lets
`PAYLOAD_FLAG_REFRACTION_PASSTHROUGH` rays through `isDeltaTransmission()` materials only.

`trySplitMaterial` only splits at roughness 0 for now (#372). Rough glass could never be split
this way, since a split on the macro-normal Fresnel would mis-weight lobes whose Fresnel is per
microfacet; other rough glossy materials simply aren't split yet.

## Shading normal

`ClosestHit_Primary` decides backfacing from the geometric normal (the interpolated normal can face
away from the ray on grazing hits, which would invert the IOR for them) and, for materials with a
glossy lobe, bends the shading normal with Cycles' `ensure_valid_specular_reflection`
(`util/shading_normal.hlsli`) so reflections never point into the surface. Water tops use the wave
normal instead; other materials keep the plain interpolated normal, flipped to face the ray. The
bent normal is shared by all of a material's lobes, so a diffuse lobe under a glossy one sees it
too, whereas Cycles bends only the specular closures' normal (a silhouette-only difference).
