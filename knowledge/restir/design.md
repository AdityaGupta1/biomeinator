_Last edited: 2026-09-03_

# ReSTIR PT Design

Target is ReSTIR PT Enhanced (Lin, Kettunen, Wyman 2026), not the original ReSTIR PT and not
ReSTIR DI. Every stage must keep the accumulated mean identical to the plain path tracer so the
existing goldens verify it. It is a sampling mode rather than a separate toggle so it can pin its NEE
strategy: it always uses RTSL, since the light tree is the importance sampler the reservoir will later
lean on for many-light scenes. Run any golden under it with `-e "--samplingMode=3"`.

## Stage 1: initial resampling over the path tree

`restir/path_tree_reservoir.hlsli`. The path tracer already produces several complete paths
per path tree (NEE hit, BSDF-sampled emissive hit, dome hit at each vertex). Instead of summing
them, each is streamed as a candidate with target `luminance(F)` and the pixel outputs
`F(selected) * W`. This has exactly the same expectation as the sum, so single-frame noise
rises but the accumulated image is unchanged.

The candidates are disjoint techniques (different path lengths, NEE vs BSDF), so their GRIS MIS
weights are all 1 and no pairwise MIS is needed at this stage. `F` is used as the path tracer
computed it: the NEE/BSDF balance heuristic is the *path* MIS weight and stays inside `F`; it
is a different thing from the *resampling* MIS weight that later stages add.

This stage is also the Enhanced paper's "unified DI+GI": the NEE ray from the primary hit is
just another candidate, so direct light will be shifted and reused like everything else.

## What is not a resampled path

These are added straight into `pathColor` and must stay outside the reservoir:

- **Primary-hit emission and the primary miss** — deterministic given the G-buffer.
- **Fog in-scatter**, both the primary segment and the two bounce segments that march
  (`pathDepth <= 1`). A fog scatter vertex would need a volumetric shift map, which the GRIS
  paper leaves as future work. The bounce-segment terms stay an independent 1 spp estimate.
  Fog *transmittance* is closed-form and part of throughput, so it lives inside `F` and is
  simply re-evaluated for any segment a shift creates, like water absorption.

## Path splitting and reservoirs

The two split threads per pixel are two path trees sampling disjoint sub-domains (the lobe at
x1 is part of a path's identity in lobe-indexed path space, and the (1-F, F) split weights are
that lobe's BSDF value with lobe pmf 1). Each thread runs its own reservoir and writes its own
slot; collect sums the slots. When a persistent reservoir buffer arrives, the two slots merge
into one canonical reservoir with MIS weights of 1, which is exact. Splitting's purpose
survives as "two initial candidate trees per pixel, one per lobe". Two reservoirs per pixel
remains a possible knob if a dim lobe under a bright one proves temporally unstable.

## RNG streams

Resampling draws use their own RNG stream (seeded separately in `RayGeneration`), never the
path's. Random replay will have to reproduce a path's BSDF and light draws exactly from its
seed, so anything that consumes the path RNG but is not part of the path parameterization
must be moved off it before replay exists: the fog march, stochastic alpha in `AnyHit`, and
Russian roulette (Enhanced applies roulette only at initial sampling, folded into the source
pdf; `F` already carries `1/survival`, which is that formulation).
