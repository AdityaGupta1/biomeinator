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
- The refraction Jacobian `ior² |wi·h| / (ior wi·h + wo·h)²` is what Cycles' `sqr(ior * inv_len_H)`
  term is. There is **no η² radiance scaling** anywhere: Cycles applies none, and the delta path
  (throughput = tint) already matches it. Value and pdf share the Jacobian, so BSDF-sampled
  throughput reduces to `G2/G1 · compensation`.
- Multiple-scattering compensation for glass uses Cycles' IOR-indexed tables
  (`ggxGlassETable` etc. in `ggx_tables.hlsli`, `16³` over roughness/cosθ/`z = sqrt((ior-1)/(ior+1))`,
  with the `Inv` pair for relative IOR < 1 looked up with `1/ior`) and the same `1 + Fms(1-E)/E`
  form as reflection, with the transmission tint as Fss. The compensation is large: at roughness 1
  the inside interface of glass has E ≈ 0.42, i.e. more than 2× boost. Verified against Cycles by
  Monte Carlo of the implemented lobe (albedo 0.893 / 0.417 vs table 0.893 / 0.422).
- Samples that end up on the wrong side of the surface for their lobe are killed (value 0, pdf 1)
  and the path continues with zero weight — kill-and-continue is unbiased, and the pdf needs no
  renormalisation because it is the true sampling density. Killed samples must still carry a valid
  direction: an uninitialised or zero direction feeds NaN geometry into the next bounce, and the
  accumulation then loses the whole pixel sample.
- `bsdfPdf` must mirror `sampleBsdf`'s lobe structure exactly (including the diffuse-transmission
  hemisphere split) or MIS silently breaks; `sampleBsdf` finishes non-delta samples through the
  same `evaluateBsdf`/`bsdfPdf` used by NEE for that reason.

## Light sampling interaction

`Material::acceptsBacksideLight()` (diffuse transmission or rough glossy transmission) is the one
predicate that tells light sampling a surface can scatter light arriving from behind the shading
normal: it widens the light-tree bounds, disables the dome-light backside rejection, and is stored
per bounce for the BSDF-hit emission MIS weight. Forward selection and pdf evaluation must use the
same value or the MIS weights disagree. Rough glass is not passthrough: the anyhit shader lets
`REFRACTION_PASSTHROUGH` shadow rays through delta transmission only.

`trySplitMaterial` does not split rough glass, since a split on the macro-normal Fresnel would
mis-weight lobes whose Fresnel is per microfacet.

## Shading normal

`ClosestHit_Primary` decides backfacing from the geometric normal (the interpolated normal can face
away from the ray on grazing hits, which would invert the IOR for them) and, for materials with a
glossy lobe, bends the shading normal with Cycles' `ensure_valid_specular_reflection`
(`util/shading_normal.hlsli`) so reflections never point into the surface. Other materials keep the
plain interpolated normal, flipped to face the ray as before.
