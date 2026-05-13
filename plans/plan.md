# Greedy Meshing Plan — Method B (binary greedy)

## Context

Mesh generation in `Chunk::createInstances` (`src/terrain/chunk.cpp`) currently emits one quad per visible block face. Instrumentation (per `greedy_log.txt`) confirms strong merge potential, especially on horizontal faces:

- **±Y faces**: avg run 2.6–3.4 per axis, ~63% of all greedy-eligible faces — biggest win.
- **±X, ±Z faces**: avg run 1.5–1.7, modest win.

Texture pipeline already supports tiled UVs: static sampler `D3D12_TEXTURE_ADDRESS_MODE_WRAP` (`src/rendering/renderer/renderer_pipeline.cpp:44`), `Vertex.uv` is full `float2` (`src/rendering/common/common_structs.h:47`), one texture per array slice. UVs outside [0,1] wrap correctly. No shader changes needed.

Greedy meshing skips water blocks and emissive (`LAMP`) blocks — they keep per-face emission as today.

## Goals

- Reduce triangle count and BLAS build cost.
- Preserve crack-free chunk boundaries (integer-derived vertex positions).
- Keep texture appearance unchanged (per-block tiling via WRAP sampler).
- Maintain area-light triangle tracking (no greedy on emissive blocks anyway).

## Out of scope

- X-shaped foliage (still 2 crossed quads with jitter).
- Water (`LIQUID_TOP` and `WATER`) — stays per-face. Water surface visuals are tightly coupled to per-block lowered top.
- Emissive blocks (`LAMP`) — stays per-face so `emissiveTriangleIdxs` keeps pointing at single-quad triangles.
- Removing existing instrumentation. Keep until both stages land and stats stop being useful.

## Eligibility

A block face is greedy-eligible iff:

- `blockData.shape == BlockShape::CUBE`
- `blockData.type != BlockType::WATER`
- `blockData.emitsLight == false`

Merge key = `(faceDir, sliceIdx)` where `sliceIdx = baseTexCoords.y * 32 + baseTexCoords.x` (matches existing emit code).

Two faces merge iff same key AND adjacent in-plane.

---

## Stage 1: horizontal faces (±Y)

### Data layout

Per chunk, work one Y slab at a time. A slab is a single Y row × full chunk XZ (16×16). Per (faceDir ∈ {+Y, -Y}, sliceIdx) build a mask:

```cpp
struct YSlabMask {
    uint16_t sliceIdx;
    uint16_t rows[16]; // bit x of rows[z] set = block (x,?,z) emits face F with this slice
};
```

- 16-bit row → `chunkSizeXZ == 16` exactly. Bit `x` ↔ X coord.
- Per slab, expect 1–4 active slice IDs (grass cap, dirt, stone, etc.). Small static array `YSlabMask masks[8]` with linear scan suffices — no map.
- Two slabs per Y level (one per face direction).

### Where mask gets populated

Hook into existing `createInstances` segment loop. For each visited block:

1. If not greedy-eligible → fall through to current emit path.
2. Else for face ±Y: compute slice; lookup-or-insert in slab-mask list keyed by `(Y, faceDir, slice)`; set bit `x` in `rows[z]`.
3. Skip emitting the per-face quad immediately.

Avoid per-Y allocations: a single `YSlabMask masks[8]` array per `(faceDir)` resets every time Y changes. Need to flush previous Y's masks → quad emission before resetting.

Iteration order: existing block loop is `z` (outer), `x`, `y` (innermost). Y is innermost which means we revisit the same slab for many (x,z). Two options:

- **Option A**: collect all greedy-eligible ±Y faces into a chunk-wide structure indexed by Y, then post-process Y by Y. Simple, larger working set.
- **Option B (recommended)**: pivot the inner loop. After the existing segment loop (which fills the mesh for non-greedy stuff), do a dedicated greedy pass that walks `for y { for z { for x { ... } } }` over `segmentsToGenerate`, building one slab at a time, flushing on Y change.

Recommend Option B: clean separation, no extra heap, easy to delete.

### Slab → quads (per slab, per slice mask)

For each `rows[16]`, extract runs:

```cpp
for (int z = 0; z < 16; ++z) {
    uint16_t row = rows[z];
    while (row) {
        int x  = std::countr_zero(row);
        int w  = std::countr_one(row >> x);
        uint16_t mask = ((1u << w) - 1u) << x;

        int h = 1;
        while (z + h < 16 && (rows[z + h] & mask) == mask) {
            rows[z + h] &= ~mask;
            ++h;
        }
        row &= ~mask;
        emitYQuad(x, y, z, w, h, faceDir, sliceIdx);
    }
}
```

`std::countr_zero` / `std::countr_one` in `<bit>` compile to `tzcnt` (BMI1, available on every target).

### Quad emission

`emitYQuad(x, y, z, w, h, faceDir, sliceIdx)`:

- Positions: corner integer offsets, scaled by (w,h). Y position is `y` (for −Y) or `y + 1` (for +Y) — same as existing per-block math but `cubeFaceVertPositions` extended span.
- UVs: `(0,0), (w,0), (w,h), (0,h)` — sampler wraps within slice.
- Normal: `(0, ±1, 0)`.
- Push two triangles, one `PerTriangleData` per triangle with `texArraySliceIdx = slice`, `flags = 0` (water excluded).
- No emissive bookkeeping (LAMP excluded).

Match existing winding (CCW seen from outside). Existing `cubeFaceVertPositions` for +Y / -Y at `chunk.cpp:547-548` defines vertex order; preserve by emitting same per-vertex pattern with scaled offsets.

### Per-chunk allocation

`uint16_t allYMasks[chunkSizeY][2][MAX_SLICES_PER_SLAB][16]` is too big. Don't allocate per chunk. Stage masks per current-Y only:

```cpp
struct SlabState {
    uint8_t numMasks;
    YSlabMask masks[8];
};
SlabState plus, minus;
```

Reset when Y advances. `MAX_SLICES_PER_SLAB = 8` — assert if exceeded (in practice 1–3). On overflow, fall back to per-face emission for excess slices.

### Verification — Stage 1

1. Build RelWithDebInfo.
2. Run `--voxelMode=true --worldSeed=4 --renderDistance=30`. Observe greedy stats counters now report only side faces (Y faces already merged out of the emit path).
3. Visual check: scroll over flat terrain, look at top/bottom of cliffs, caves. No texture stretching, no missing faces, no z-fight.
4. Stat: log new `[greedy] Y-pass merged X faces → Y quads (ratio = ...)` once per chunk to confirm reduction matches expected ~2.6–3.4×.
5. Run voxel mode tests in `src/tests/` to ensure existing screenshots match (or update goldens).

---

## Stage 2: vertical faces (±X, ±Z)

### Bounded to one segment at a time

Per data, side-face Y-axis runs average ~1.5. Capping Y-merge at segment height (8) loses <5%. Memory cost stays trivial.

### Data layout per segment per face direction

Segment = 4×8×4 blocks.

- **±X faces**: live on YZ planes at fixed X. 4 X-slices per segment. Per slice mask = 4(Z) × 8(Y) bits → fits in `uint32_t`.
- **±Z faces**: live on XY planes at fixed Z. 4 Z-slices. Per slice mask = 4(X) × 8(Y) bits → fits in `uint32_t`.

Structure per face direction:

```cpp
struct SegmentSideMask {
    uint16_t sliceIdx;
    uint32_t planes[4]; // plane index = X (for ±X) or Z (for ±Z)
                        // each uint32_t = 4 rows × 8 bits, row r at bits [r*8 .. r*8+7]
};
```

Small static array `SegmentSideMask masks[8]` per (segment, face direction).

### Where mask gets populated

Process each segment in `segmentsToGenerate` separately. After Stage 1's chunk-wide ±Y pass has flushed its own quads, run a per-segment vertical pass:

1. For each greedy-eligible block face ±X/±Z in segment, set bit in `planes[planeIdx]` at `(row=Z or X) * 8 + (col=Y)`.
2. After segment scan, run binary greedy per `(faceDir, plane)`:
   - Treat the 32 bits as 4 rows of 8.
   - Outer iterate rows; for each row extract Y-runs by `countr_zero` / `countr_one` on the 8-bit row.
   - Inner loop: extend across adjacent rows of same `mask`.
3. Emit one quad per merged region using the same integer-positioning logic, scaled.

### Quad emission specifics

±X faces:
- Positions span Y for `w_run`, Z for `h_run`. UV (w_run, h_run) for tile wrap.
- Normal `(±1, 0, 0)`.

±Z faces:
- Positions span X for `w_run`, Y for `h_run`. UV (w_run, h_run).
- Normal `(0, 0, ±1)`.

Preserve winding per `cubeFaceVertPositions[faceIdx*4..faceIdx*4+3]`.

### TRANSPARENT_CUTOUT note

Cutout-cutout face emit rule (`shouldGenerateFace` returns true only when `all(thisPos <= neighborPos)`) means side faces of leaves blocks at leaves-leaves boundaries emit only on the lower-positioned side. Greedy still works — merging cutout-cutout interior faces is consistent within slice. Leaves visual unchanged.

### Verification — Stage 2

1. Build RelWithDebInfo.
2. Run same flags. Inspect tall structures (oak/palm/acacia trunks, cliff faces, sand-stone walls).
3. Cross-segment Y boundary check: at every Y multiple of 8, expect quad splits — this is correct per design. No visual seam (positions still integer).
4. Toggle Stage 2 on/off via a temporary `debugBool` for A/B comparison until satisfied.
5. Drop the run-length instrumentation once both stages validated and metrics no longer needed.

---

## Files to modify

- `src/terrain/chunk.h` — declare new private methods: `greedyMeshYFaces`, `greedyMeshSideFaces`, helper `flushYSlab`.
- `src/terrain/chunk.cpp` — split greedy-eligible emit out of main loop; add Stage 1 then Stage 2.
- `src/terrain/terrain.cpp` — no change (already forwards `threadMemoryAlloc`).
- Optional: lift integer face geometry helpers to a small free function to share between greedy and non-greedy paths.

## Files NOT to modify

- Shaders (`src/shaders/materials/materials.hlsli`, terrain pipelines) — already sampler-wrap correct.
- Sampler / pipeline setup (`renderer_pipeline.cpp`) — already WRAP.
- `Vertex` / `PerTriangleData` structs — already supports float2 UV and per-triangle slice.
- Water / emissive block paths — explicitly excluded.

## Risk + rollback

- Per-stage gating via a `debugBool` keeps fallback path one toggle away during shake-out.
- Quad winding bugs → easy to spot (back-face cull or hot path-trace artifacts). Test by flipping normals briefly.
- Cutout block visual regression unlikely (same emit rule, scaled UV) but eyeball leaves/cactus first.

## Knowledge-base updates (after both stages land)

- Update `knowledge/terrain/greedy_meshing.md` (currently says "not traditional greedy meshing despite the filename" — that becomes inaccurate).
- Note the per-segment cap on vertical merges (non-obvious; would surprise a reader).
- Note that water and emissive deliberately keep per-face emission and why.
