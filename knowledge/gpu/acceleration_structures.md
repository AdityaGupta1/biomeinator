_Last edited: 2026-08-22_

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

## BLAS Refit

Deformable BLASes (`BlasBuildInputs::allowUpdate`, currently water) are built once with
`ALLOW_UPDATE`, then refit in place (source == dest) every frame by `updateBlases` after the
displacement pass rewrites their verts. Refit is valid because topology/vert count never
change — only vertex Y moves. Gotchas: `ALLOW_UPDATE` must also be passed to the prebuild
info query or `UpdateScratchDataSizeInBytes` comes back 0, and refit flags must match the
original build's flags aside from `PERFORM_UPDATE`.

`updateBlases` issues a UAV barrier on `sharedAcsBuffer` **before** the refits: last frame's
`DispatchRays` read these BLASes and the in-place refit writes the same memory (the buffer
lives permanently in the AS state, so ordering is UAV-barrier-only). Fresh-section BLAS
builds never need this because they write virgin memory.

## TLAS UAV Barrier

`makeTlas` inserts a UAV barrier on `sharedAcsBuffer` before building the TLAS. This ensures
all BLAS builds (which wrote to the same buffer) are complete before the TLAS reads them.
Without this barrier, the TLAS could read partially-built BLASes.
