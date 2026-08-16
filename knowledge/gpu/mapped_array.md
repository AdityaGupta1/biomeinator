_Last edited: 2026-08-16_

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

## Per-Frame Upload Staging

Writes through `operator[]` land directly in the mapped upload buffer, with no
synchronisation against the GPU. With a single upload buffer that is a CPU/GPU race: the CPU
can run up to `NUM_FRAMES_IN_FLIGHT - 1` frames ahead, so frame N+1's writes overwrite the
buffer that frame N's not-yet-executed `CopyBufferRegion` still has to read, and the device
buffer receives a mix of two frames' data.

`perFrameUpload` allocates one upload buffer per frame in flight and indexes it by
`Renderer::getFrameIndex()`. A slot is only rewritten once its frame context comes round
again, by which point `beginFrame` has already waited on that frame's fence, so its copy has
completed. No extra synchronisation is involved.

The device buffer stays single under this option: the GPU is its only accessor, and the
copy's state transitions already order the write against the shader read. Duplicating it
would also force re-uploading unchanged data into each copy.

**Precondition:** only valid for arrays that rewrite every live element on each dirty cycle.
An array updated element-wise through `markDirty` would find stale values in whichever slot
it lands on, trading an intermittent race for deterministic staleness.

`areaLightSamplingStructure` is the array that needs this, because the light tree's
`emitter_collect` indexes its UAVs with values read straight out of it — a torn upload
becomes an out-of-bounds GPU write and a page fault, not merely a wrong-looking frame. The
other single-buffered arrays (`mappedInstanceDatasArray`, `mappedMaterialsArray`) carry the
same race but update element-wise, so they cannot adopt this as-is; their corruption
surfaces as a one-frame visual artefact instead.

## Resize Marks Everything Dirty

`resize()` copies old data via `memcpy` on the mapped pointer, then marks the entire new
range dirty. Old buffers are pushed to `ToFreeList`. After resize, the next
`copyFromUploadBufferIfDirty` copies everything — there's no way to know which entries the
GPU already had.
