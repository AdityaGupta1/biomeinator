_Last edited: 2026-09-20_

# ReservedManagedBuffer

See [managed_buffer.md](managed_buffer.md) for the full design and comparison with
`CommittedManagedBuffer`.

## Why Reserved Resources

Growth requires no data copy — just mapping a new heap into the existing virtual address
space via `UpdateTileMappings`. This matters for the large geometry buffers (verts, idxs,
per-tri data, acceleration structures) where copying gigabytes on resize would stall the
pipeline.

## Growth Cost

`CreateHeap` for a growth chunk took 15-40 ms on the main thread, which during world streaming
(a new 64 MB heap every few frames for the BLAS buffer) was the largest single source of frame
spikes. Two things address it: heaps are created `CREATE_NOT_ZEROED`, which took the call to
well under a millisecond, and the heap after the one just mapped is created on a background
thread (`std::async`) so that even a slow creation is off the frame. `init` does not prefetch,
so buffers that never grow cost nothing extra.

## Constraints

Cannot be mapped (no CPU access) and must be marked resizable. The `maxReservedSizeBytes`
virtual size is fixed at construction and cannot change — running out of virtual space is a
fatal assert. The graphics queue must be initialized before `init()` because
`UpdateTileMappings` is a queue operation.
