_Last edited: 2026-09-04_

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

## Stage 2: replayable paths and the reservoir buffer

`PathReservoir` (shared struct in `common_structs.h`, one per pixel slot in `dev_reservoirs`) stores
what rebuilding the selected path anywhere needs: the path seed, length, technique, reconnection
vertex index, the rc vertex itself, the direction leaving it, the radiance arriving along that
direction, and the light pdf for MIS. `pathTraceRay` is one loop with two modes: initial sampling
feeds candidates to the reservoir; replay rebuilds one stored path from its seed and returns its
integrand. Keeping both in one function is what guarantees they agree.

**Vertex indexing follows the papers**, x1 = primary hit, x_k = light vertex, and passthrough hits
are not vertices. Every candidate is identified by (k, technique, j) with j the rc vertex (0 = none).
The four techniques (NEE area, NEE dome, BSDF-hit emission, BSDF miss to dome) are disjoint
domains. A reservoir's `F` excludes the Russian roulette division: the resampling weight is still
`luminance(F_withRR)`, since that already equals `pHat / p_source` with roulette folded into the
source pdf, and the shading output is unchanged because the roulette factor cancels in `F * W`.

**Reconnection data semantics.** `rcRadiance` is everything after the rc vertex x_j *excluding* the
segment x_{j-1} to x_j, which replay re-traces (visibility, passthrough tint, absorption, fog). When
j == k (rc is the light vertex, always the case for NEE paths per Enhanced 6.2.3, and for BSDF light
hits when x_{k-1} passes the criteria) it is the raw emission and replay recomputes both pdfs. When
j == k - 1 it is emission times the final segment's transmittance, with the path MIS weight left out
so replay can recompute it from its own bsdf pdf against the stored `rcLightPdf`. Otherwise it
includes the MIS weight and every factor from x_j's scatter onward. With their MIS weights applied,
NEE and BSDF sampling of a light vertex both reduce to `f cos Le / (p_light + p_bsdf)`, which is why
replay treats them identically once reconnected.

**Reconnection criteria** (`restir/reconnection.hlsli`) are Enhanced's, not the 2022 thresholds:
single-vertex roughness of the sampled lobe at x_{j-1} (>= 0.2, so delta lobes never reconnect) and
the dual footprint test against `0.0002 * primary footprint`. The inverse test is skipped for
diffuse-only and light vertices. The decision is made at x_j *after* its BSDF sample (the inverse
test needs that pdf) and *before* x_j's light samples, since those paths reconnect at x_j too.
`BsdfSample::lobeRoughness` exists for this.

**What replay reproduces exactly and what it does not.** The self-replay debug modes
(`--restirDebugMode=1|2`) re-trace each pixel's own stored path and either shade with it or show
the relative error (x100). With `--rngSeed` fixed, mode 0 and mode 1 renders of a glTF scene
compare pixel-exact except for a handful of pixels; voxel worlds cannot be compared across runs at
low frame counts because chunk streaming timing differs, so use the error view for those. Known
inexact cases: rough glass, where `evaluateBsdf`'s half-vector reconstruction occasionally disagrees
with the sampled one at float precision (sparse dots on rough spheres); grazing hits on smooth
curved surfaces, where the re-aimed reconnection ray can graze the surface before the target;
water absorption on a reconnection segment, which uses the shadow-ray entry/exit formulation.

**Reconnection ray TMax.** An NEE light point carries its triangle's geometric normal, so the ray
can stop short of the light's *plane* like `traceToLight` (the offset origin makes the plane
crossing earlier than the distance at oblique angles). A BSDF-sampled hit only has its shading
normal, and using that plane made rays self-occlude on the target (evil_room's specular-coated
objects). Those rays are instead aimed from the offset origin exactly at the target and stopped at
distance minus epsilon, which reproduces the original closest-hit ray.

## Stage 3: paired spatial reuse

Three passes per frame in ReSTIR PT mode: initial sampling (raygen, one thread per pixel slot),
the spatial shift (the same raygen with `PtPass::SPATIAL_SHIFT` in a root constant, one thread per
pixel), and the resample compute pass, which also does the shading. The pass index is a root
constant because the global params cbuffer is uploaded once per frame.

**Shift pass.** Each pixel first merges its two split slots into one canonical reservoir (exact,
unit MIS weights, deterministic per pixel and frame so partners recompute the same merge), then for
every pairing texture shifts its partner's path to itself: replay from this pixel's primary hit with
the partner's stored path. Because the pairing is symmetric, that single evaluation is both this
pixel's candidate from the partner and the partner's MIS term for this pixel's own path. Results go
to `dev_shifted`, one `ShiftedPath` per pixel per texture.

**Shift definition.** Replay at a foreign pixel *is* the hybrid shift. Two checks make it
invertible (GRIS 7.4): no pair on the offset path before the stored rc vertex may pass the
reconnection criteria, and the pair (y_{j-1}, x_j) must pass them; the base path's lobe type at
x_{j-1} must exist at y_{j-1} and supplies the roughness. NEE light vertices are exempt from the
second check since NEE paths always reconnect to their light. A path stored without reconnection
is likewise undefined wherever the offset path would reconnect. Undefined shifts return zero and
the MIS weights hand their share to the canonical sample, so they cost variance, never bias.

**Jacobian.** Enhanced Eq. 2 in primary sample space: the shifted path's pdf and geometry terms
across the reconnection over the base path's. The base terms are stored as one product
(`rcJacobianTerms`); the technique's pdf at a light vertex is the light pdf for NEE paths and the
BSDF pdf otherwise, the dome has no geometry term, and a final NEE segment contributes no pdf at
the rc vertex since light sampling ignores the incoming direction. A shifted path that gets
selected carries its own terms forward.

**Resample pass.** Confidence-weighted defensive pairwise MIS (GRIS Eq. 38 with each pHat times its
reservoir's M, verified to sum to 1). The partner's view of the canonical path,
`pHat_partner(T^-1(x_c)) * |dT^-1|`, is luminance of this pixel's path shifted to the partner times
that Jacobian; the canonical's view of the partner's path is luminance of the partner's stored F
over the Jacobian of the shift into this pixel. Partners on screen count toward the neighbor
confidence whether or not their shift succeeded. The result is written back into slot 0 of
`dev_reservoirs` (the initial slots are dead after the shift pass) and shaded into the raw buffer.

**Verification.** `--restirDebugMode=3` pairs every pixel with itself. With partner == self the
weights are 7/16 for the canonical and 3/16 per neighbor, all with the same pHat and W, so the output
must equal `--restirSpatialNeighbors=0` pixel-exact; and if a self-shift fails a criteria check at
float precision, the canonical MIS term absorbs it and the output is still exact. This is the check
that the MIS, merge and Jacobian plumbing cancel correctly. With real partners, goldens must still
match in mean, since spatial reuse within a frame is unbiased.

## Stage 4: temporal reuse

A `TEMPORAL` raygen pass between initial sampling and the spatial shift. It merges the pixel's
split slots (canonical, M = 1), reprojects the current primary hit into the previous frame with
`worldToPrevClipMat` and the previous jitter, validates the previous G-buffer hit there (same
surface within a few pixel footprints, normals agree), and pairs the canonical with that pixel's
history reservoir under the same pairwise MIS as spatial reuse. The history is confidence-capped
(`restirTemporalConfidenceCap`, 20 by default), so M saturates at 21 after temporal and about 84
after three spatial partners; the `CONFIDENCE` debug view shows M / 100.

Both shifts of the pair are real replays: the history path is rebuilt at this frame's pixel, and
this pixel's path is rebuilt at the previous frame's pixel from the previous G-buffer and camera
position. `pathTraceRay` therefore takes the camera position as a parameter and derives the primary
direction from the hit in replay mode. Both replays trace against the current scene, the standard
approximation.

**Buffers that cross frames** ping-pong by frame parity: the G-buffer (`dev_gbuffers`) and the
history reservoirs (`dev_reservoirsHistory`), which the spatial resample pass writes. History is
valid only if the previous frame ran ReSTIR reuse and neither the scene nor path tracing settings
changed; camera motion is handled by reprojection, resizes reset the frame counter.

**Color noise.** Resampling selects by luminance, so `F * W` of the selected path carries one
path's chroma however many candidates were merged: with temporal and spatial reuse the luminance
error of a single frame fell 2.5x while the chroma error rose. The spatial resample pass therefore
shades with the vector-valued weights of Enhanced 6.3, the RGB sum of every candidate's
`mis * F * W * J`, which has the same expectation and averages the partners' independent chroma.
The reservoir itself still resamples on scalar weights. Debug views: `CONFIDENCE` (M / 100) and
`SHIFT_SUCCESS` (temporal and spatial shift success rates, partners on screen).

**Reprojection.** A pixel is identified by its *area* in the previous frame, `floor(prevUv *
size)`, never by the jittered sample position. Subtracting the previous jitter made a still camera
pull history from the up-left neighbor whenever this frame's jitter was smaller, and at 20:1
confidence that reads as the whole image swimming down-right. Positions stored last frame (history
rc vertices, the previous G-buffer, the previous camera) are moved into this frame's render space by
`prevGlobalInstanceOffset - globalInstanceOffset` before use, so the voxel floating origin can move
between frames.

**Moving-camera validation** uses the `scriptedCamera*` settings (deterministic per-frame
translation and yaw for N frames, then the camera holds), a converged RTSL reference accumulated at
the final pose, and single frames captured on the first static frame, whose history was built
entirely through the motion. Because the accumulation counter resets on every camera change, that
first static frame is what `--antialiasingMode=0 --maxAccumulatedFrames=1` captures. Gains under
motion match the stationary gains; the `SHIFT_SUCCESS` view averages over the whole frame, so keep
the scene in view or the empty pixels dominate it. Animated geometry still silently invalidates
stored rc vertices, and disocclusions get no history.

## RNG streams

Every draw comes from `initRng(pathSeed, index, purpose)` with purposes for BSDF, NEE area, NEE dome,
their shadow rays, fog, the traced ray's anyhit, and roulette (`PATH_RNG_*` in the rgs). BSDF and
light purposes are keyed by vertex index, so replay can reproduce one vertex's draw without
re-running anything before it and passthrough hits on one path do not shift the streams of the
other. Ray and fog streams are keyed by loop iteration, which is deterministic per path but can
differ between a base and an offset path across passthrough surfaces (only alpha decisions and fog
jitter are affected). Shadow rays have their own streams so a reconnection ray to an NEE light
vertex replays the same alpha decisions. Resampling draws are a separate stream seeded in
`RayGeneration`.

## Memory

Per pixel: 2 x 112 B initial reservoirs, 112 B merged, 2 x 112 B history, 3 x 32 B shifted paths
and a second 64 B G-buffer, allocated regardless of sampling mode. Enhanced compresses reservoirs to 64 bytes; that is a later, measured
step, and the self-replay error view is the tool to measure what precision loss costs.

## Storing world positions

`rcHit` stores world-space position and shading normal rather than barycentrics, because
`InstanceData` has no full object-to-world matrix on the GPU (glTF instances can scale and rotate).
The normal is stored as oriented for the original incoming ray; replay flips it (and the backface
IOR) when reconnecting from the other side. Voxel mode's floating origin (`globalInstanceOffset`)
means stored positions go stale when it changes; temporal reuse must handle that.
