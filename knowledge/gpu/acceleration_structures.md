_Last edited: 2026-08-02_

# Acceleration Structures

`src/rendering/buffer/acs_helper.h/cpp` — BLAS and TLAS construction.

## Geometry Flags

All BLAS geometry is built with `NO_DUPLICATE_ANYHIT_INVOCATION` (stored in
`GeometryWrapper::geometryFlags` so refits reuse the same flags): the anyhit shader mutates
the payload (passthrough tint, stochastic alpha rng draws), which the spec allows to be
invoked multiple times per triangle per ray without this flag.

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
