_Last edited: 2026-04-26_

# ReservedManagedBuffer

See [managed_buffer.md](managed_buffer.md) for the full design and comparison with
`CommittedManagedBuffer`.

## Why Reserved Resources

Growth requires no data copy — just mapping a new heap into the existing virtual address
space via `UpdateTileMappings`. This matters for the large geometry buffers (verts, idxs,
per-tri data, acceleration structures) where copying gigabytes on resize would stall the
pipeline.

## Constraints

Cannot be mapped (no CPU access) and must be marked resizable. The `maxReservedSizeBytes`
virtual size is fixed at construction and cannot change — running out of virtual space is a
fatal assert. The graphics queue must be initialized before `init()` because
`UpdateTileMappings` is a queue operation.
