# Branch review findings

_Last edited: 2026-09-20_

## Scope and validation

- Reviewed branch: `better_blas_builds` at `8752945f5a3b221437759c7e05e2702498b917c0`.
- Comparison base: `origin/main` at `aa3f30b4c89638308485c04a67ee2918d4ea48b2`.
- Focus: correctness, potential p95/p99 spikes, and avoidable repeated work comparable to the previously fixed per-chunk water dispatches.
- The review was read-only. These notes do not implement fixes.
- Validation consisted of static tracing and in-memory models, including 15,000 randomized cases checking the compaction range/gather algorithm. Those models passed; they do not validate the actual GPU implementation or synchronization.
- No GPU profiling or application runs were performed. Performance findings identify stall mechanisms or redundant work, not measured regressions. Source line numbers below refer to the reviewed commit.
- Not every finding was introduced by this branch. Retained behavior and additional inherited issues are identified below.

The clearest remaining sources of avoidable spikes are compaction-buffer growth and unnecessary light-tree rebuilds.

## Actionable findings

### 1. [P2] The new compactor allocates GPU resources during chunk unloads

Location: `src/rendering/area_light_compactor.cpp:103`, with allocation at lines 122-124; growth implementation in `src/rendering/buffer/committed_managed_buffer.cpp`.

The compactor starts scratch at 1 MiB. Compacting roughly two million indices needs about 8 MiB. Consecutive unload frames can grow it to 16/32 MiB because previous allocations remain pending until their frame contexts retire. Each growth synchronously creates a committed resource, copies old scratch contents, and schedules another resource release.

This reintroduces the allocation/copy/release pattern this branch otherwise works to remove. The old scratch contents are disposable, so preserving them is unnecessary work.

Suggested fix: pre-size or reuse scratch, account for in-flight allocations if retaining the current allocator, and avoid preserving old scratch contents on growth. A persistent scratch design must retain the required GPU ordering.

Validation target: first broad unload in a loaded world with approximately two million lights, followed by consecutive unload frames. Check both resource creation and deferred resource release costs.

### 2. [P2] Removing non-emissive geometry still rebuilds the entire light tree

Location: `src/scene/scene.cpp:847-849`; downstream gate at `src/rendering/light_tree_manager.cpp:409`.

TLAS compaction unconditionally sets `areaLightTopologyChanged`, even when only water or other non-emissive instances disappeared. That triggers clearing, emitter collection, sorting, and rebuilding all light-tree levels despite the emitter set being unchanged.

This is retained redundant work, not a newly introduced regression. It is a close analogue to the per-chunk dispatch problem and fits the scope of this branch.

Suggested fix: mark lighting dirty only when the emitter set actually changes. Track removed emissive entries rather than checking only whether compaction ranges moved: removing an emissive entry from the tail must still invalidate lighting.

Validation target: remove a non-emissive instance from a scene that contains lights and confirm the light-tree build is skipped; separately remove the last emissive block and confirm lighting is invalidated.

### 3. [P2] Vertical movement does not invalidate the cached water animation set

Location: `src/scene/scene.cpp:905-918`, especially the camera-position assignment at line 907. Membership uses camera Y at lines 935-952.

`setWaveFrustum` updates camera position without invalidating `animatedDeformables`. Other invalidation covers XZ chunk changes and orientation, but membership also depends on camera Y. Descending straight toward water can change its correct membership from excluded to fully animated while the cached set remains unchanged.

An in-memory reproduction using the default 35-degree vertical FOV found a water patch excluded with the camera high above it and fully animated after a vertical descent, with unchanged XZ and frustum normals. The cache invalidation inputs do not capture this change.

Suggested fix: include bounded vertical movement in cache invalidation, with a conservative margin if quantizing movement.

Validation target: finish loading, keep orientation and XZ fixed, then descend toward water. Water should enter the animation set without requiring a turn or horizontal movement.

### 4. [P2] Water motion vectors omit displacement caused by changing the fade

Location: `src/shaders/path_tracing/gbuffer.rgs.hlsl:59-61`.

The shader calculates:

```text
(previousHeight - currentHeight) * currentFade
```

The fade-dependent displacement difference needs to be:

```text
previousHeight * previousFade - currentHeight * currentFade
```

Moving or turning changes displacement even with animation paused, when the existing expression returns zero. For example, equal wave heights of 0.06 with a fade change from 1.0 to 0.4 produce an omitted vertical delta of 0.036. This gives DLSS incorrect motion around fade boundaries.

Suggested fix: preserve previous fade parameters and use the previous and current displaced positions consistently.

Validation target: pause animation and move through the radial fade, or turn through the angular fade. Verify the deformation component of the motion vectors remains correct.

### 5. [P2] The combined water dispatch has an unchecked size limit

Location: `src/rendering/water_displacer.cpp:169`. The same unchecked pattern appears at `src/rendering/area_light_compactor.cpp:141`.

The combined water dispatch submits every vertex along X. With 64 threads per group, more than 4,194,240 vertices exceeds the standard 65,535-group X limit. Larger animation distances can reach this aggregate size even though the former per-chunk dispatches were individually small. The compactor has the same issue for a sufficiently large compacted tail.

Some devices support larger 1D dispatches, but this path does not query or gate on that capability. The concern applies to devices with the standard limit.

Suggested fix: flatten a 2D dispatch, use bounded batches, or capability-gate larger 1D dispatches. Ensure shader indexing follows the selected approach.

Reference: [Microsoft ID3D12GraphicsCommandList::Dispatch documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-dispatch).

Validation target: counts immediately below and above 4,194,240, including a large water animation radius and any flattening work added when chunks leave the animated set.

### 6. [P2] The configured BLAS maximum cannot be lowered below eight

Location: `src/scene/scene.cpp:580-586`.

The clamp silently raises `maxBlasBuildsPerFrame=1...7` to eight. This prevents the obvious way to trade streaming throughput for lower spikes, despite the setting being described as the maximum number of builds per frame.

Suggested fix: make the proportional floor respect the explicitly configured maximum, and define the behavior of a zero setting explicitly.

Validation target: a backlog of 64 instances with a configured maximum of one must not submit eight builds.

### 7. [P2] Moving perf comparisons measure different routes on faster/slower builds

Location: `src/rendering/renderer/renderer_perf.cpp:242-250`, together with elapsed-time movement in `Camera::processInput` and the fixed-frame measurement window.

Movement uses real elapsed time, while measurement ends after a fixed number of frames. At 20 blocks/s over 1,500 frames, 12 ms frames cover approximately 360 blocks, while 9 ms frames cover approximately 270 blocks. Different views and chunk boundaries therefore confound the comparison.

Suggested fix: add a deterministic camera path indexed by measured frame for controlled A/B measurements. A real-time fixed-speed mode can remain useful, but it should not be mistaken for replaying the same per-frame workload.

Validation target: compare camera positions and streaming events by measured frame across runs with different frame times.

### 8. [P2] Reflex sleep timing is recorded and immediately discarded

Location: `src/rendering/renderer/renderer.cpp:509-521`; clearing occurs in `CpuProfiler::beginFrame` at `src/rendering/cpu_profiler.cpp:36-40`.

The Reflex sleep scope is recorded before `beginFrame()`, which clears the profiler's timings. The report therefore cannot attribute pacing spikes to this wait, even though the scope exists in source.

Suggested fix: begin CPU scope collection before Reflex sleep while keeping the intended distinction between CPU work time and pacing waits.

Validation target: a perf run with Reflex available should contain a `reflex sleep` CPU scope.

## Additional existing paths worth addressing in this branch

### A. Large light-tree allocations still happen synchronously

Location: `src/rendering/light_tree_manager.cpp:320` and `:355`; additional allocations in `src/rendering/gpu_sort/gpu_radix_sort.cpp:184`.

The light-tree manager grows multiple substantial buffers, with additional radix-sort allocations. Pre-sizing the sampling-index array does not pre-size these buffers. Crossing capacity boundaries remains a strong candidate for isolated large hitches, including the later release of old resources.

Consider pre-sizing, warming, or preparing these capacities outside latency-sensitive streaming frames, with an explicit memory budget. Profile allocation boundaries rather than only steady-state tree build time.

### B. Heap prefetch can still block the render thread

Location: `src/rendering/buffer/reserved_managed_buffer.cpp:56` and `:71-82`.

`future.get()` is blocking and is called at exhaustion. The first growth has no prefetched heap, and size mismatches allocate synchronously. Prefetch improves typical cases but does not bound stalls if allocation is slow or demand catches up with the background request.

Consider triggering prefetch before exhaustion and deferring eligible streaming work when capacity is not ready. This is a conditional stall mechanism, not a measured regression in this review.

### C. Inherited correctness issue: light-tree bounds retain an old camera origin

Location: `src/shaders/light_tree/emitter_collect.cs.hlsl:36`; rebuild gate at `src/rendering/light_tree_manager.cpp:409`; sampling uses current hit positions in `src/shaders/common/light_tree_sampling.hlsli:212` and `:288`.

Emitter collection builds camera-relative bounds, but the tree only rebuilds on emitter topology changes. Moving the camera changes the origin used by shading without translating those stored bounds. Importance estimates then drift, and reachable lights can receive zero light-selection probability. BSDF sampling still exists, so this finding does not establish that all contributions from those lights disappear; it identifies incorrect light-selection geometry and potential noise/flicker.

Suggested fix: track the tree's build origin and transform sampling and PDF query positions into that coordinate system. This avoids requiring a complete light-tree rebuild whenever the camera origin changes.

Validation target: move horizontally in an already loaded emissive scene without changing emitter topology. Verify light-tree geometric weights remain consistent with the actual light positions.
