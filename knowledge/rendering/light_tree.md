_Last edited: 2026-05-17_

# Light Tree Build

Stages 1–3 of the Real-Time Stochastic Lightcuts plan (`plans/plan.md`). Builds
a GPU perfect-binary tree over emissive triangles, rebuilt on every area-light
topology change. Stage 4 (path-tracer sampler) is what consumes it; this entry
covers everything up to but not including that consumer.

Owned by `LightTreeManager` (`src/rendering/light_tree_manager.{h,cpp}`),
dispatched from `renderer.cpp` inside the `hasTlas()` gate. Uses the GPU radix
sort wrapper documented in [gpu_radix_sort.md](gpu_radix_sort.md).

## Why a separate per-light extras buffer

`AreaLight` already exists (sparse-keyed `StructuredBuffer<AreaLight>`) but
holds only the data the path tracer reads at shade time (vertices, instance
id, material id). The tree needs world-space bbox + flux per emitter that no
other consumer cares about — `LightAux` (parallel sparse-keyed buffer) keeps
them out of the hot path's working set.

## Sparse-key the aux buffer, dense-key the tree

`dev_lightAux` / `dev_lightToLeaf` size to the **sparse high-water mark**
(`Scene::getAreaLightSparseCount()`) so the cheap math at Stage 4's MIS BSDF-hit
recovery — `instanceData.areaLightsBufferOffset + perTriData.localAreaLightIdx`
— indexes them directly. `dev_lightTree` and the Morton buffers size to
`M = nextPow2(numAreaLights)` (dense). Two capacity fields, two
`ensureCapacity` paths — do not conflate.

## Sentinel encoding lets every pass skip live-slot branches

- **Aux sparse holes** (slots not pointed to by `areaLightSamplingStructure`)
  carry `bboxMin = +∞`, `bboxMax = -∞`, `flux = 0`, written by the Stage 1
  `light_buffer_clear` pass. The bbox-reduce shader unions them in
  unconditionally — `min(+∞, real) = real`, no liveness check needed.
- **Bogus tree leaves** (slots `[numAreaLights, M)` after the leaf-populate
  pass) share the same sentinel + `areaLightIdx = LIGHT_IDX_INVALID`. The
  internal-levels gather sums and unions them with no branch; their
  contribution is identity.

This is why Stage 1 invests in a separate `light_buffer_clear` dispatch instead
of relying on emitter_collect to also zero unused slots — sparse holes carry
the sentinel between rebuilds.

## `dev_sceneBbox` and the orderable-uint atomic float trick

The bbox-reduce pass is a workgroup-shared reduce followed by a
cross-workgroup atomic min/max into a 24-byte scene bbox buffer (3 mins, 3
maxes). D3D12 has no native atomic float min/max. We encode each float to a
uint that preserves IEEE ordering across the sign boundary:

```
non-negative f → asuint(f) | 0x80000000   (positives sort high)
negative     f → ~asuint(f)               (larger-magnitude negatives sort low)
```

That mapping is bijective and monotone, so `InterlockedMin/Max` on the encoded
uint gives the same result as a float comparison. Decoded back via
`orderableUintToFloat` (see `src/shaders/common/light_tree.hlsli`). Buffer is
bound as `RWByteAddressBuffer` because `InterlockedMin` lives there, not on
typed UAVs.

A separate single-thread dispatch (`light_tree_scene_bbox_reset.cs.hlsl`)
seeds the 6 slots with `floatToOrderableUint(±∞)` at the start of every
rebuild — atomics only narrow toward correctness, so the start state must be
the widest possible.

## Why `depth=2` fused internal-levels dispatch

The naive bottom-up tree gather is one dispatch per level (`log2(M)` total). We
fuse two levels per dispatch: each thread reads 4 grandchildren, unions them
pairwise to compute 2 intermediate children, writes both, then unions those to
write the parent. Saves a UAV barrier per pair of levels and halves the
dispatch count.

`log2(M)` is sometimes odd (`M ∈ {512, 2048, 8192, ...}`) — the C++ loop drops
to `depth=1` on the last dispatch via a root-constant flag the shader branches
on. No write contention either way: each thread owns a disjoint
(parent, 2 children, 4 grandchildren) tuple.

## Why 30-bit Morton instead of 32

10 bits per axis (`uint3(t * 1024)` clamped to 1023). Leaves the top 2 bits of
the 32-bit Morton code zero so the result sorts cleanly as an ascending uint32
radix key without sign-bit surprises. Position normalization uses
`saturate((centroid - sceneMin) / extent)`; centroid rollover at `t == 1` is
clamped explicitly because `uint(1.0 * 1024)` rounds to 1024, one past the
valid range.

## HLSL gotcha: `centroid` is a reserved word

`centroid` is the HLSL interpolation modifier (used in PS inputs for
centroid-sampled interpolants). Naming a parameter `float3 centroid` makes the
parser interpret `float3` as the type, `centroid` as a modifier expecting
*another* type to follow — emits the confusing `modifiers must appear before
type` error pointing at the next token. Use `pos` / `centerPos` / anything
else.

## DXC `#pragma once` does not normalize relative paths

DXC HLSL was deduping `common_structs.h` only by literal include-string match.
A `.cs.hlsl` shader that includes both `../../rendering/common/common_structs.h`
*and* `../common/light_tree.hlsli` (which itself includes the same header via
`../../rendering/common/common_structs.h`) sees two different relative-path
strings resolving to the same file. The shader CMake didn't canonicalize.
Result: every struct in `common_structs.h` redefined, cascading parse errors.
Belt-and-suspenders: `common_structs.h` carries both `#pragma once` *and* an
explicit `#ifndef COMMON_STRUCTS_H` guard.

## Build-trigger ergonomics

`Scene::didAreaLightTopologyChange()` is the single source of truth for "the
tree is stale". Stage 1 and Stage 2 both consume it; both rebuild together
because Stage 2 is a pure function of Stage 1's output. When dynamic emitter
transforms or runtime emissive-strength changes land later, expanding *that*
trigger (e.g. `didAreaLightContentChange()`) automatically rebuilds the tree
too — no separate Stage 2 trigger to keep in sync.

## Dispatch count and resource pattern

`4 + ceil(log2(M)/2)` Stage 2 dispatches per rebuild (scene bbox reset, bbox
reduce, morton emit, leaf populate, then the internal-levels loop), plus 4
internal sort passes from `GpuRadixSort`. At M=256: 4 internal-level
dispatches; at M=1M: 10. Build-only buffers (`dev_lightAux`, `dev_sceneBbox`,
`dev_mortonKeys`, `dev_mortonValues`) stay in
`D3D12_RESOURCE_STATE_UNORDERED_ACCESS` and use UAV barriers only between
writer→reader passes (matches Stage 1's pattern). The two path-tracer-visible
buffers (`dev_lightTree`, `dev_lightToLeaf`) round-trip
UAV → NON_PIXEL_SHADER_RESOURCE each frame via
`LightTreeManager::transitionForPathTracingRead` — only for buffers actually
written that frame; skipped buffers rely on D3D12 buffer state decay back to
COMMON and implicit-promote to SRV on raygen read.
