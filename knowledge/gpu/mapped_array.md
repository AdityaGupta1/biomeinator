_Last edited: 2026-04-26_

# MappedArray

`src/rendering/buffer/mapped_array.h` — typed CPU-visible array with dirty tracking and
partial GPU upload. Used for per-frame data that changes sparsely (instance data, materials,
area light sampling indices).

## Upload-Only Mode

When `uploadOnly` is true, no device buffer is created — the upload buffer IS the final
buffer. Used for instance descriptors (`D3D12_RAYTRACING_INSTANCE_DESC`) which are read
directly from the upload buffer by TLAS build, so a device-side copy would be wasted work.

## Dirty Range Merging

`insertDirtyRange` maintains a sorted, non-overlapping list of dirty ranges and merges
adjacent/overlapping entries. `copyFromUploadBufferIfDirty` only copies dirty ranges rather
than the full buffer. This matters when only a few entries change per frame out of thousands.

## Resize Marks Everything Dirty

`resize()` copies old data via `memcpy` on the mapped pointer, then marks the entire new
range dirty. Old buffers are pushed to `ToFreeList`. After resize, the next
`copyFromUploadBufferIfDirty` copies everything — there's no way to know which entries the
GPU already had.
