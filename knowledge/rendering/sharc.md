_Last edited: 2026-09-09_

# SHaRC

The standalone NVIDIA shader library is pinned in `external/SHARC`. Host ownership lives
in `renderer/renderer_sharc.cpp`. The renderer keeps the reference pipeline and builds
update/query variants of the same iterative path tracer. Interactive rendering enables
SHaRC on supported devices; headless tests/perf default off to preserve reference goldens.
The existing SM 6.9 requirement covers int64
atomics on root-descriptor structured buffers; native fp16 is checked separately.

## Scheduling and storage

After G-buffer, jittered sparse update paths reuse primary hits. Update disables primary
path splitting and guide/output writes. Resolve runs once per cache entry, followed by
normal full-resolution tracing with immediate cache lookups. Collect/DLSS remain unchanged.
All three cache buffers stay in UAV state, with explicit barriers between clear, update,
resolve and query. Cache resources are persistent across frames, not ping-ponged; a single
ordered graphics queue serializes their accesses. Retired allocations use the frame's
ToFreeList so capacity changes never free in-flight storage.

The default 2^20 entries cost 40 MiB (8-byte hashes, 16-byte accumulation and resolved
entries). Runtime counter collection, atomics, logging and per-frame readbacks have
been removed. Historical measurements below predate their removal.

## Transport invariants

Only diffuse outgoing radiance is cached. Material demodulation preserves base-color
texture detail. Emission is separate: a query supplies zero emission and the renderer
adds the actual hit emission with its existing previous-vertex MIS weight. NEE must not
also run at a successfully queried vertex. Update keeps local direct light separate from
emission, propagating the MIS-weighted latter only into preceding entries.

Update resets local radiance and throughput at every vertex, folding segment weights into
SHaRC's stored sample weights. Non-diffuse vertices propagate contributions without
creating cache entries. Delta transport remains exact; update clamps non-delta roughness
as a bias/noise tradeoff. Update skips Russian roulette; the normal rendering path retains it.
Cache resampling uses the SDK's bounded propagation history. Consequently the cached tail
is not a strictly depth-limited estimator: compare against a sufficiently deep reference.

Queries exclude primary hits, glossy/passthrough targets and short segments. The incoming
accumulated cone diameter must exceed two voxel widths. Glossy scattering and refraction
propagate the cone across bounces, including sharp bounces after an earlier rough event.
Guide outputs for primary and specular look-through are produced before query termination.

Incoming-segment fog and water attenuation are evaluated before queries. Update does not
store camera-to-primary fog as surface lighting; inter-vertex in-scatter is propagated to
preceding entries. Fog is depth-limited in the reference shader, so caching can approximate
its depth dependence. Dynamic lighting and geometry may retain history for several frames.

Display changes (SHaRC view, antialiasing mode, accumulation limit) restart image
accumulation without clearing the cache. Radiance-affecting UI settings, scene changes,
cache configuration changes and explicit reset still invalidate it. Hash-grid and cached-
radiance views bypass beauty tracing and initialize their path-produced guides explicitly.

## Coordinates and history

The renderer shifts its origin with the camera. Cache positions instead use a fixed anchor:
`hitPos_WS + (globalInstanceOffset - cacheOrigin)`. Current/previous cache camera positions
share that frame. Ordinary renderer-origin shifts do not clear the cache. Re-anchor and
clear after 2048 units to bound quantization/float error. Scene/topology changes, relevant
settings, manual reset, capacity changes, and re-enabling clear the cache. Camera motion
alone resets image accumulation, not cache history.

For accumulation/screenshot runs, an explicit cache warmup keeps resetting image
accumulation until the cache has received the requested number of update frames. This
prevents cold-cache frames from contaminating a warmed comparison.

## Validation

`--testRadianceOutput=<path>.pfm` alongside testOutput exports the raw linear RGB result,
combining path splits and normalizing accumulation, before tonemapping or DLSS. No lossy
image conversion is involved. `--rngSeed` gives reproducible sampling; zero retains random
seeding. Compare mean energy and non-bright pixels separately, then inspect the images:
small global error alone does not rule out local leakage.

Use the existing performance runner with matched scenes/settings.
Compare total frame time, not just the path-tracing scope: update and resolve are real costs.
The uncached path can win on scenes with little reusable diffuse transport.

## Debug views

`--sharcDebug=4` displays cached outgoing scattered radiance at primary surface hits,
including material remodulation. This intentionally bypasses normal query eligibility
to expose individual cache cells. Missing entries and uncached pure emitters are black.
`--sharcDebug=3` instead assigns each spatial/normal hash cell a diagnostic color; it
shows grid geometry even where no entry has been populated. Modes 1 and 2 display
query success and traced bounce count. Debug views are not suitable DLSS guide inputs;
use accumulation (`--antialiasingMode=1`) to inspect them.

Modes 5–11 instead decompose the beauty estimator after tracing, without changing its
sampling or query decisions: 5 is primary-surface NEE (area and dome lights), 6 is the
actual throughput-weighted cache contribution, 7 is MIS-weighted emission reached by
the first secondary ray (including a sky miss), 9 is later NEE, 10 is later emitter/sky
hits, 11 is primary visible surface emission, and 8 is the remaining volume/primary-sky
radiance.
Mode 7 uses literal path depth, so passthrough intersections can move direct emission
into later emission. The seven components retain both path splits and sum to beauty
before tonemapping, up to floating-point subtraction error.
These modes use a separate diagnostic raygeneration pipeline so the extra component
accumulators are compiled out of the ordinary query and reference shaders.

The local analysis rendered components at 1 spp and 256 spp after 256 warmup frames,
with path splitting and jitter disabled, and measured single-sample MSE against the
accumulated SHaRC result.
Component MSE shares are a noise diagnostic, not exact shares of total beauty variance:
they omit covariance, and the accumulated target still contains noise and cache bias.

The 2026-09-09 breakdown at 960×540, default RTSL, depth 16, 256 warmup frames showed:

| Component MSE share | cave_lights | crystal_caves |
|---|---:|---:|
| Primary NEE | 25.4% | 1.9% |
| First-secondary-ray emission | 73.1% | 15.2% |
| Cached contribution | 1.0% | 0.3% |
| Later NEE | included in <0.5% remainder | 1.3% |
| Later emission | included in <0.5% remainder | 81.3% |

Primary visible emission is effectively noiseless in these fixed-camera captures.
Crystal caves' noisy glass pillars appear in the later-emission component. Thus high
cache hit rate does not imply a large noise reduction: explicit emitter-hit contributions
remain outside the diffuse cache. These figures describe this integration and settings,
not an inherent limit of radiance caching. Artifact metrics are in
`build/sharc_breakdown/combined_metrics.json`; the finer crystal breakdown is in
`build/sharc_breakdown_detail/`.

Direct ray-frequency counters (same 960×540 settings, no path splitting; three warmed
logged frames per scene) measured primary-BSDF emitter-hit rates of 4.06% in cave_lights,
5.17% in crystal_caves, 0.88% in Cornell, and 5.06% in evil_room. These are fractions of
launched primary-surface BSDF rays, not noise shares or fractions of all pixels.
Raw counts are in `build/sharc_primary_hits/metrics.json`.

## Accumulated cone termination

Forced-query experiment flags and their shader paths have been removed. Queries now
require an incoming ray-cone diameter greater than two voxel widths, plus the existing
segment-length guard (sqrt(3) voxel widths). Primary hits and glossy targets remain excluded.
The shared texture ray cone grows during travel and scatters after sampling the BSDF,
including the primary bounce. Sampled diffuse lobes add a broad spread; glossy lobes use
the guide's GGX effective diameter spread. Independent spreads combine in quadrature.
Transmission applies a locally planar Snell derivative to width/angular spread and scales
microfacet broadening relative to reflection. The narrower refraction axis is used
conservatively. This scalar approximation ignores curvature and anisotropy; it is not
an exact propagation of the full GGX distribution. Lookup remains a single-cell lookup.
Both SHaRC-on and off use the same revised texture footprint.

Paired 512-spp,
480x270 unsplit/no-jitter renders after 256 warmup frames gave mean radiance errors
of -0.023% crystal_caves, -0.024% cave_lights, -0.473% Cornell, and -0.078% evil_room.
All radiance values were finite/nonnegative. Results: `build/sharc_cone_validation`.
960x540 warmed individual 1-spp pairs: `build/sharc_cone_1spp`; Downloads copies use
`04_cone_test__` in `SHaRC-renders-2026-09-09`. Crystal averaged 328864 cache hits/frame
(vs roughly 322k before), cave_lights 453460 (roughly unchanged). This is a modest
increase in crystal cache usage, not a large visible improvement to glass noise.

For a stable view of the default grid, render the Cornell scene with `--sharc=true
--sharcDebug=4 --antialiasingMode=1 --maxAccumulatedFrames=256 --sharcWarmupFrames=256
--noJitter --frameGeneration=false --testOutput=build/sharc_validation/cache_radiance.png`.

## Historical primary-glass measurements

The now-removed diagnostics counted resolved primary glossy-transmission hits, BSDF
rays launched from those hits, successful cache terminations, and unique paths that
attempted an eligible lookup. A path stayed in the cohort regardless of initial
reflection or transmission. Counts were per path, not energy weighted.

Crystal caves, current accumulated cone, 960x540, no splitting/jitter, seed 1738:
last four logged frames after 256 warmup frames averaged 154991 primary-glass paths,
154872.75 launched rays, 97918.25 paths attempting a lookup, and 96035.25 successful
cache terminations. That is 61.962% of primary-glass paths (62.009% of launched rays),
with 98.077% of eligible paths eventually finding the cache. This includes termination
at any later diffuse bounce, not necessarily immediately after exiting glass.
Raw measurements and settings: `build/sharc_primary_glass/metrics.json`.
