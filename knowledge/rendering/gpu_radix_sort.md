_Last edited: 2026-05-15_

# GPU Radix Sort

Wraps the external GPUSorting library (`external/GPUSorting`, MIT) to provide an
in-place sort of 32-bit uint keys + 32-bit uint values, ascending. First caller
is the Stage 2 light tree build (RTSL, see `plans/plan.md`); future Morton/
depth-sorting passes can reuse it.

## Why GPUSorting / DeviceRadixSort

- D3D12 + HLSL out of the box, no CUDA dependency.
- Pure compute shaders — fits the renderer's existing PSO/root-sig pattern.
- DeviceRadixSort (reduce-then-scan) is the portable variant; OneSweep
  (chained-scan-with-decoupled-lookback) is faster but relies on forward
  thread-progress guarantees we don't want to assume across hardware.
- AMD FidelityFX Parallel Sort was the fallback plan; not needed.

## Tuning is baked at shader-compile time

`SortCommon.hlsl` has compile-time `#define` switches for keys-per-thread,
threads-per-block, partition size, and shared memory. GPUSorting's runtime
`Tuner.h` picks values per device (NVIDIA shared-mem class, RDNA, etc.). We
hardcode the NVIDIA pairs preset for the 131072 B/SM tier (RTX 30/40 series):

| Macro            | Value | Source                          |
|------------------|-------|---------------------------------|
| `KEYS_PER_THREAD`| 15    | default in `SortCommon.hlsl:22` |
| `D_DIM`          | 512   | default in `SortCommon.hlsl:28` |
| `PART_SIZE`      | 7680  | default in `SortCommon.hlsl:40` |
| `D_TOTAL_SMEM`   | 7936  | default in `SortCommon.hlsl:46` |

These match the default branches in the shader, so the CMake compile rules
pass *no* tuning overrides — only `-DSORT_PAIRS -DKEY_UINT -DPAYLOAD_UINT
-DSHOULD_ASCEND -DENABLE_16_BIT`. **If a future GPU class needs different
tuning, add the appropriate `-DPART_SIZE_xxxx` style override to the CMake
shader-compile invocation, not to the C++ code.** The C++ side has no concept
of the partition size other than the matching `PARTITION_SIZE` constant in
`gpu_radix_sort.h`, which **must be kept in sync** with whatever `PART_SIZE`
the shader resolves to.

## Register slots are hardcoded by upstream

GPUSorting's HLSL hardcodes register slots: `cbGpuSorting : register(b0)`,
`b_sort u0`, `b_alt u1`, `b_sortPayload u2`, `b_altPayload u3`, `b_globalHist
u4`, `b_passHist u5`. We can't remap without forking upstream, so
`gpu_radix_sort.cpp` builds root signatures with raw register numbers rather
than the codebase's `MAKE_PARAM` / `_REGISTER_*` enum scheme. Constants for
those slots live as `GPU_SORT_REG_*` `constexpr`s in the anonymous namespace
of `gpu_radix_sort.cpp` — search for those if upstream renumbers anything.

## Ping-pong invariant (why the API is in-place)

DeviceRadixSort is LSD over 8-bit digits → 4 passes for a 32-bit key. Each
Downsweep scatters from a `sort` buffer to an `alt` buffer; the next pass
swaps roles. With an even pass count (4), the result lands back in the
*input* buffer. We rely on this and expose an in-place API: caller passes a
single keys/values buffer pair, scratch alt buffers are owned by
`GpuRadixSort` (`dev_altKeys`, `dev_altValues`).

If a future change moves to a 5-byte or 3-byte key (odd radix pass count),
the final result will land in the alt buffer — the dispatcher would need a
final `CopyBufferRegion`. The current code asserts and assumes the 4-pass
contract.

## Scratch buffer lifecycle

- `dev_globalHist` (4 KiB, `RADIX * RADIX_PASSES * 4 B`) — allocated once in
  `init()`, never resized. Init kernel zeroes it at the start of every
  dispatch.
- `dev_altKeys` / `dev_altValues` — grow in pow2 steps from a 1024 floor when
  `numKeys` exceeds current capacity. Old buffers pushed onto the caller's
  `ToFreeList` (same pattern as `LightTreeManager::ensureCapacity`).
- `dev_passHist` (`RADIX * threadBlocks * 4 B`) — grows in pow2 steps over
  `threadBlocks` independently of `scratchCapacity`. Sized in thread-block
  count rather than key count because the underlying need rounds up to the
  partition boundary.

## Single-dispatch cap

GPUSorting's kernel sources have logic for splitting a >65535-block Upsweep/
Downsweep across two dispatches (the `isPartial` flag in `cbGpuSorting`).
We don't wire that yet — `MAX_KEYS = 65535 * PART_SIZE ≈ 503M` is plenty for
the renderer's current and foreseeable needs. The dispatcher asserts the
limit. If a future caller wants more, see
`DeviceRadixSortKernels.h:88-120` for the split-dispatch pattern to port.
