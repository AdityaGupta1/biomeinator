_Last edited: 2026-09-09_

# SHaRC

The standalone NVIDIA shader library is pinned in `external/SHARC`. Host ownership lives
in `renderer/renderer_sharc.cpp`. The renderer keeps the reference pipeline and builds
update/query variants of the same iterative path tracer; the query variant carries every ReSTIR
PT raygen so the reuse passes see the cache as initial sampling does (see
[restir → design.md](../restir/design.md), "Radiance cache termination"). Interactive rendering enables
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
entries).

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
accumulation without clearing the cache. Radiance-affecting UI settings, scene/world replacement, material/texture changes,
cache configuration changes and explicit reset still invalidate it. Hash-grid and cached-
radiance views bypass beauty tracing and initialize their path-produced guides explicitly.

## Coordinates and history

The renderer shifts its origin with the camera. Cache positions instead use a fixed anchor:
`hitPos_WS + (globalInstanceOffset - cacheOrigin)`. Current/previous cache camera positions
share that frame. Ordinary renderer-origin shifts do not clear the cache. Re-anchor and
clear after 2048 units to bound quantization/float error. Ordinary voxel chunk streaming
and visibility changes preserve cache history; affected cells refresh through updates
and stale-entry eviction. Image accumulation still resets on those scene changes.
Scene/world replacement and material/texture uploads set a pending invalidation flag
that SHaRC consumes once to clear the cache. Non-voxel topology edits, relevant settings, manual reset, capacity
changes, and re-enabling also clear it. Camera motion alone preserves cache history.
Voxel edits share the streaming path and converge through cache updates rather than a
global clear, so lighting can briefly lag behind edited geometry.

For accumulation/screenshot runs, an explicit cache warmup keeps resetting image
accumulation until the cache has received the requested number of update frames. This
prevents cold-cache frames from entering the accumulated image.

## Debug views

`--sharcDebug=4` displays cached outgoing scattered radiance at primary surface hits,
including material remodulation. This intentionally bypasses normal query eligibility
to expose individual cache cells. Missing entries and uncached pure emitters are black.
`--sharcDebug=3` instead assigns each spatial/normal hash cell a diagnostic color; it
shows grid geometry even where no entry has been populated. Modes 1 and 2 display
query success and traced bounce count. Debug views are not suitable DLSS guide inputs;
use accumulation (`--antialiasingMode=1`) to inspect them.

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
