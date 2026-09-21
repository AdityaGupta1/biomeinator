_Last edited: 2026-09-20_

# Acceleration Structures

`src/rendering/buffer/acs_helper.h/cpp` — BLAS, TLAS, and OMM Array construction.

## Geometry Flags

BLAS geometry defaults to `NO_DUPLICATE_ANYHIT_INVOCATION` (stored in
`GeometryWrapper::geometryFlags` so refits reuse the same flags): the anyhit shader mutates
the payload (passthrough tint, stochastic alpha rng draws), which the spec allows to be
invoked multiple times per triangle per ray without this flag. `BlasBuildInputs::isOpaque`
switches to `OPAQUE` instead — used for terrain chunks with no cutout faces, whose anyhit
would only ever conclude alpha == 1 (the terrain material has no transmission, so no
passthrough behavior is lost).

## Opacity Micromaps

There is a single OMM Array (built once by `buildOmmArray`, terrain cutout tiles; see
[terrain → terrain_omm.md](../terrain/terrain_omm.md)); its GPU VA is kept in a static so
`makeBlasBuildInputs` can link any BLAS whose `GeometryWrapper` has a valid
`ommIdxsBufferSection` without the generic Scene/AcsHelper layers knowing about terrain.
Ordering: `buildOmmArray` issues the UAV barrier on `sharedAcsBuffer` itself, so any later
BLAS build (same or later command list) safely dereferences the array. The OMM Array result
is sub-allocated from `sharedAcsBuffer` like BLASes/TLAS (spec allows intermixing); its
128-byte alignment requirement is met because every section in that buffer is rounded to
256 bytes. The R16 OMM index buffers live in the scene's idxs buffer — safe because the
buffer's `alignmentBytes` rounds every section size to a multiple of 4, keeping all
offsets aligned.

Traversal that can encounter OMM-linked triangles without opting in is undefined behavior,
so the opt-in is set in both places whenever OMMs are active: pipeline flag in
`makeRtPipeline`, and the `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS` template flag on the fog
RayQuery (always set there — harmless when nothing is linked).

## Shared Buffers

All acceleration structures (BLAS and TLAS) are sub-allocated from a single
`ReservedManagedBuffer` (`sharedAcsBuffer`). Scratch space is sub-allocated from a separate
`CommittedManagedBuffer` and freed to `ToFreeList` after each build. Upload staging for
vertex and index data uses two more committed buffers.

## Build Inputs Need Not Stay Resident

An acceleration structure holds its own copy of the geometry, so the vertex buffer a build reads
is only needed until the build has run. Instances with `host_packedTerrainVerts` exploit this:
the build reads fp32 positions straight from the section in `sharedVertsUploadBuffer` (an
upload heap is permanently `GENERIC_READ`, which covers the required
`NON_PIXEL_SHADER_RESOURCE` state, and the section lives on through the free list for the
frames in flight), while only the packed copy is uploaded to the resident verts buffer. Refit
BLASes cannot do this because `updateBlases` re-reads the resident vertices.

## BLAS Refit

Deformable BLASes (`BlasBuildInputs::allowUpdate`, currently water) are built once with
`ALLOW_UPDATE`, then refit in place (source == dest) every frame by `updateBlases` after the
displacement pass rewrites their verts. Refit is valid because topology/vert count never
change — only vertex Y moves. Gotchas: `ALLOW_UPDATE` must also be passed to the prebuild
info query or `UpdateScratchDataSizeInBytes` comes back 0, and refit flags must match the
original build's flags aside from `PERFORM_UPDATE`.

`updateBlases` takes one scratch allocation for the whole batch (sub-allocated at 256-byte
steps) rather than one per BLAS; with a thousand water refits a frame the per-refit free-list
traffic was measurable on the main thread. It issues a UAV barrier on `sharedAcsBuffer`
**before** the refits: last frame's
`DispatchRays` read these BLASes and the in-place refit writes the same memory (the buffer
lives permanently in the AS state, so ordering is UAV-barrier-only). Fresh-section BLAS
builds never need this because they write virgin memory.

## BLAS Compaction

Static BLASes (everything but water) are built with `ALLOW_COMPACTION` and copied into a
section of their compacted size a few frames later; on the RTX 4070 SUPER this took terrain
BLASes from ~77 to ~21 bytes per triangle (1.9 GB to 0.5 GB at render distance 30), and it
is what the `blasCompaction` setting toggles. The compacted size is only known after the
build has run, so the round trip is spread across the frame contexts:

- The build itself writes a `COMPACTED_SIZE` postbuild info entry (passed to
  `BuildRaytracingAccelerationStructure` directly, so no barrier sits between build and
  query), and the batch copies the entries to a readback buffer. Both buffers belong to a
  `BlasCompactionQuery` owned by the frame context, sized for the per-frame build cap and
  grown only by the glTF load path, which builds everything in one batch.
- When that frame context comes around again its fence has passed, so `Scene::update` reads
  the sizes and records `CopyRaytracingAccelerationStructure(COMPACT)` into a fresh section
  *before* this frame's builds. The old section goes to `ToFreeList` because in-flight frames
  still trace through it; `makeTlas`'s UAV barrier already orders the copies before the TLAS
  build.

Instances are recycled, so an `Instance*` captured at build time can hold a different chunk
three frames later. `GeometryWrapper::blasBuildId` is stamped per build and the compaction is
dropped when it no longer matches (or the section was freed, or the instance is scheduled for
deletion). Water BLASes are never compacted: refit BLASes compact poorly and they are under
100 MB in total.

The `ALLOW_COMPACTION` flag itself showed no build- or trace-time difference back to back,
and the copies cost well under 0.1 ms per frame while streaming. Compacted sections are
smaller than the holes they leave, so the AS buffer's `allocated - used` gap in the memory
report is the fragmentation to watch.

## TLAS UAV Barrier

`makeTlas` inserts a UAV barrier on `sharedAcsBuffer` before building the TLAS. This ensures
all BLAS builds (which wrote to the same buffer) are complete before the TLAS reads them.
Without this barrier, the TLAS could read partially-built BLASes.
