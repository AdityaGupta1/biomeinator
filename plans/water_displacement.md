# Water Wave Displacement

Real geometry displacement of WATER_TOP top vertices via a compute shader, with analytic
normals derived from the same closed-form wave function.

## Wave function

New shared header `src/shaders/common/water_waves.hlsli` (usable by both the displacement
compute shader and hit shaders):

```hlsl
// Two summed diagonal sine waves; coefficients hardcoded for now.
// A1 + A2 = 0.1 (max displacement +/- 0.1 blocks)
// INVARIANT: total amplitude must stay < 0.125 — non-top water verts sit at integer Y,
// which is exactly 0.125 from the nearest k + 7/8 rest position, so the in-place
// vertex identification below breaks if displacement can reach 0.125.
float waveHeight(float2 posXZ_WS, float time)
{
    return 0.06f * sin(dot(posXZ_WS, float2(0.8f, 0.6f)) + 1.1f * time)
         + 0.04f * sin(dot(posXZ_WS, float2(-0.5f, 1.3f)) + 1.7f * time);
}

float2 waveGradient(float2 posXZ_WS, float time); // closed-form cosine partials
// surface normal = normalize(float3(-grad.x, 1.f, -grad.y))
```

Evaluated at **world-space** XZ. Chunk transforms are pure integer translations, and rest
vertex coordinates are integer + 7/8, so world XZ is exact float math — identical inputs for
shared boundary verts across chunks, no seams. The gradient is translation-invariant, so the
same normal formula works from `hitPos_WS`.

## Vertex identification & in-place update

Rest Y of every displaceable vert is `k + 7/8` (guaranteed by the committed topYSubtract
fix); all other water verts sit at integer Y. An integer vert `y = k` reconstructs to
`restY = k - 0.125` (`round(k - 0.875) = k - 1`), i.e. it appears exactly **0.125** from the
nearest rest position — that is the real ambiguity threshold. With max amplitude 0.1, top
verts show `|distToRest| <= 0.1` and integer verts always show exactly 0.125, so the compute
shader can work **in place** with no rest-verts copy:

```
distToRest = y - (round(y - 0.875) + 0.875)   // |distToRest| < 0.11 -> top vert
restY      = round(y - 0.875) + 0.875
newY       = restY + waveHeight(posXZ_WS, time)
```

The classification threshold is set strictly between maxAmplitude (0.1) and 0.125 — use
0.11 — so peak-displacement verts have float slack and integer verts still fail cleanly.

Non-top verts (integer Y) fail the distance test and are left untouched. Side-face top verts
are at `k + 7/8` too, so they move with the surface automatically. Normals in the vertex
buffer are never modified (side faces keep flat normals).

## Changes by area

### 1. Time source

- Add `float time` (elapsed seconds) to `RenderParams` (`common_params.h`), accumulated from
  delta time in `renderState`, set alongside `frameNumber` in `renderer.cpp` (~line 561).
- The displacement pass and hit shaders must both consume this **same** value each frame,
  or normals won't match geometry.
- `// TODO: float precision of elapsed seconds degrades after hours (~1 ms resolution at
  ~4.6 h); wave phase gets steppy in long sessions. Wrap time periodically if it matters.`

### 2. Verts buffer UAV access (`scene.h`)

- Add `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` to `managedVertsBuffer`'s
  `bufferCreationFlags`.
- Per frame, around the displacement dispatch: transition
  `NON_PIXEL_SHADER_RESOURCE -> UNORDERED_ACCESS`, dispatch, UAV barrier, transition back
  (BLAS build/refit reads verts in `NON_PIXEL_SHADER_RESOURCE`).

### 3. Deformable instance tracking

Scene has no notion of "deformable instance" today; chunks own `waterInstance` pointers.
Naming stays generic because leaves/grass will become deformable later — only the wave
function and displacement shader are water-specific. Add a flag to `Instance` (e.g.
`isDeformable`, set in `chunk.cpp` where the water instance gets its material, ~line 760).
Scene keeps an `unordered_set<Instance*>` of finalized, BLAS-built deformable instances
(e.g. `deformableInstances`):

- inserted after the instance's BLAS is first built in `makeQueuedBlases`;
- erased in `freeInstance`.

This set drives both the displacement dispatches and the BLAS refits. For now every
deformable instance is water; when other deformable types arrive, the set can either be
split per type or the Instance can carry a deformation-type enum to pick the dispatch
shader.

### 4. Displacement compute pass

New `src/shaders/water/water_displace.cs.hlsl` (register via `shaders.cpp` +
`REGISTER_SHADER`, PSO alongside the other compute PSOs — follow the light-tree pattern in
`light_tree_manager.cpp`):

- Inputs per water instance (root constants or small cbuffer): verts buffer offset (in
  verts), vert count, `transformOffset.xz`, time.
- One dispatch per water instance; thread = one vertex. Reads `Vertex` from the verts UAV,
  applies the identify/displace logic above, writes back.
- Water instances are small (2^8 verts reserve); per-instance dispatch overhead is fine to
  start. TODO in code: batch into a single dispatch with an instance table if it shows up in
  profiling.

### 5. BLAS refit (`acs_helper.h/cpp`)

- Add `bool allowUpdate` to `BlasBuildInputs`; when set, build with
  `ALLOW_UPDATE | PREFER_FAST_TRACE` and store `UpdateScratchDataSizeInBytes` in
  `GeometryWrapper`. (`PREFER_FAST_TRACE`: water BLASes are built once, refit forever, and
  hit by many primary/refraction rays — spec's "fastest trace against update-able AS" case.)
- The `ALLOW_UPDATE` flag must also be passed to the
  `GetRaytracingAccelerationStructurePrebuildInfo` inputs, or `UpdateScratchDataSizeInBytes`
  comes back 0.
- Set it for deformable instances in `Scene::makeQueuedBlases`.
- New `AcsHelper::updateBlases(cmdList, toFreeList, dev_verts, wrappers)`: for each
  wrapper, rebuild the geometry desc from its existing verts/idxs sections and issue
  `BuildRaytracingAccelerationStructure` with flags
  `ALLOW_UPDATE | PREFER_FAST_TRACE | PERFORM_UPDATE`, source == dest (in-place), scratch
  from `sharedAcsScratchBuffer` sized by the stored update scratch size. Per spec, update
  flags must match the original build's flags aside from `PERFORM_UPDATE`, and `ALLOW_UPDATE`
  must stay set or no further updates are allowed.
- UAV barrier on `sharedAcsBuffer` **before** the refit writes: the previous frame's
  `DispatchRays` read these BLASes, and in-place refit writes the same memory (the buffer
  lives permanently in the AS state, so ordering is UAV-barrier-only). The existing barrier
  in `makeTlas` only covers this frame's writes -> TLAS read, not last frame's read -> this
  frame's write. (Fresh-section BLAS builds never hit this because they write virgin memory.)
- Note: refit is valid because topology/vert count never changes; only Y moves by <= 0.1.

### 6. Per-frame flow in `Scene::update`

Order within the existing cmdList:

1. `makeQueuedBlases` (unchanged — newly built deformable BLASes get built from rest
   positions; they'll be refit next frame, or this frame if included in step 3's set
   already).
2. Displacement pass: transition verts buffer to UAV, dispatch per deformable instance
   (water shader for all of them today), UAV barrier, transition back.
3. `AcsHelper::updateBlases` over all tracked deformable instances (preceded by the
   UAV barrier on `sharedAcsBuffer` from section 5 — last frame's rays read these BLASes).
4. TLAS: **rebuild unconditionally every frame** (drop the `isTlasDirty` gate; keep the
   flag only if other logic depends on it). This is required for correctness once BLASes
   deform (TLAS caches BLAS AABBs), and should also smooth the frame-pacing spikes caused by
   bursty rebuilds.
   - `// TODO: use TLAS PERFORM_UPDATE instead of full rebuild on frames with no new/freed
     chunks` (the `updateScratchSizePtr` plumbing in `makeTlas` is the start of this).

The existing UAV barrier in `makeTlas` on `sharedAcsBuffer` already covers refit-writes ->
TLAS-read ordering.

### 7. Analytic normals on water hits

New per-triangle flag to mark wave-surface faces, so the hit shader doesn't have to infer
top-ness from the interpolated normal (fragile — interpolated Y is a barycentric sum that
isn't exactly 1):

- `common_structs.h`: add `TRIANGLE_FLAG_IS_WATER_TOP (1 << 1)` next to
  `TRIANGLE_FLAG_IS_WATER`.
- `chunk.cpp` face loop (~line 727): set it alongside `TRIANGLE_FLAG_IS_WATER` when
  `isWater && faceIdx == 4` (`faceOffsets[4] = (0, 1, 0)` is the top face); side faces get
  only `TRIANGLE_FLAG_IS_WATER`.

In `path_tracing_common.hlsli` closest-hit only — `gbuffer.rgs.hlsl` reads the
already-computed `payload.hitInfo.hitNor_WS` from ClosestHit_Primary, so the override there
flows into the gbuffer automatically, no separate change:

- Condition: `(perTriData.flags & TRIANGLE_FLAG_IS_WATER_TOP)`. Side faces lack the flag,
  keep their flat normals.
- Replace the normal with `normalize(float3(-grad.x, 1, -grad.y))` where
  `grad = waveGradient(hitPos_WS.xz, time)`.
- `hitPos_OS` interpolation already returns displaced positions (verts updated in place),
  so hit positions are consistent for free.
- AnyHit water entry/exit absorption tracking is unaffected.

## TODOs to leave in code/plan

- Motion vectors for displaced water (currently none -> DLSS/temporal ghosting on water).
- TLAS `PERFORM_UPDATE` path for frames with no chunk churn.
- Batch displacement dispatches if per-instance dispatch cost matters.
- Float time precision: wrap elapsed-seconds periodically so wave phase doesn't get steppy
  in multi-hour sessions.

## Known impacts / risks

- **Voxel goldens**: water surface geometry now depends on `time`; goldens containing water
  will diff. Check how goldens pin time (frame 0 / time 0 still displaces since
  `waveHeight(x, z, 0) != 0`). May need golden regen or a setting to disable waves for
  goldens.
- Whole-resource state transitions on the shared verts buffer also cover terrain verts;
  harmless but means the displacement pass must not overlap other passes reading verts.
- Amplitude cap: in-place reconstruction breaks if total amplitude reaches 0.125 (integer
  verts sit exactly 0.125 from the nearest rest position); coefficients live in one header
  with a comment stating the invariant. Current 0.1 leaves only 0.025 margin.

## Verification

- Run app, observe waving water; check chunk borders for seams (shared verts must move
  identically).
- Toggle debug view of normals to confirm analytic normals match geometry (no swimming
  highlights).
- Confirm no D3D12 debug-layer errors (states, refit).
- Run a small representative subset of goldens to scope the golden diff before deciding on
  regen vs. disable-waves flag.
