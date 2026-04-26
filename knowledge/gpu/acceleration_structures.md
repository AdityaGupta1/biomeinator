_Last edited: 2026-04-26_

# Acceleration Structures

`src/rendering/buffer/acs_helper.h/cpp` — BLAS and TLAS construction.

## Shared Buffers

All acceleration structures (BLAS and TLAS) are sub-allocated from a single
`ReservedManagedBuffer` (`sharedAcsBuffer`). Scratch space is sub-allocated from a separate
`CommittedManagedBuffer` and freed to `ToFreeList` after each build. Upload staging for
vertex and index data uses two more committed buffers.

## BLAS Build Flag

All BLASes use `PREFER_FAST_BUILD` over `PREFER_FAST_TRACE`. This is a tradeoff for terrain:
chunks are created frequently and the faster build time is worth the slightly slower trace.

## TLAS UAV Barrier

`makeTlas` inserts a UAV barrier on `sharedAcsBuffer` before building the TLAS. This ensures
all BLAS builds (which wrote to the same buffer) are complete before the TLAS reads them.
Without this barrier, the TLAS could read partially-built BLASes.
