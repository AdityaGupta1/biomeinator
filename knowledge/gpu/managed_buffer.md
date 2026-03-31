_Last edited: 2026-03-31_

# ManagedBuffer

`src/rendering/buffer/managed_buffer.h/cpp` — a GPU buffer with a built-in free-list allocator. Rather than one buffer per allocation, large buffers are shared and sub-allocated from. `ManagedBuffer` is the base class; `ReservedManagedBuffer` and `CommittedManagedBuffer` are the two concrete implementations.

## Free-List Allocator

The allocator tracks free regions using two mirrored maps kept in sync at all times:
- `freeByOffset` — keyed by byte offset, used to find and merge adjacent free blocks on deallocation.
- `freeBySize` — keyed by size, used for O(log n) best-fit lookup on allocation.

**Allocation** (`findFreeSection`) — finds the smallest free block that fits, carves out the requested size, and returns a `ManagedBufferSection` (offset + size + pointer back to the owning buffer). If no block fits and the buffer is resizable, `ensureCapacity` is called first.

**Deallocation** (`freeSection`, called via `ManagedBufferSection::free()`) — returns the section to the free list and merges it with adjacent free neighbors to prevent fragmentation.

**`ManagedBufferSection`** is a lightweight handle — offset, size, and a raw pointer to the owning `ManagedBuffer`. `getGpuVirtualAddress()` adds the offset to the buffer's base GPU VA. Sections should always be freed via `ToFreeList` rather than directly, to avoid freeing while the GPU is still reading them.

## Copying Data In

Three copy paths, chosen based on whether the source is on CPU or GPU:

- **`copyFromHostBuffer` / `copyFromHostVector`** — `memcpy` into a mapped buffer. Only valid when `isMapped` is true.
- **`copyFromDeviceBuffer` / `copyFromManagedBuffer`** — `CopyBufferRegion` on the command list. Transitions the buffer to `COPY_DEST` and back automatically, unless a batch copy is active.
- **Batch copy** (`beginBatchCopy` / `endBatchCopy`) — holds the buffer in `COPY_DEST` state across multiple `copyFromDeviceBuffer` calls, avoiding redundant barriers. Must be balanced and only valid for unmapped buffers.

## The Two Implementations

### ReservedManagedBuffer

Uses D3D12 *reserved resources* (tiled resources). On init, a single `ID3D12Resource` is created at the full `maxReservedSizeBytes` virtual size — this reserves address space but commits no physical memory. Physical memory is backed by a collection of `ID3D12Heap` objects that are mapped into the virtual space on demand via `UpdateTileMappings`.

When more capacity is needed, a new heap is created and mapped immediately after the current physical end — no reallocation or data copy required. Growth is rounded up to 32 MB chunks.

**Constraints**: cannot be mapped (no CPU access), must be resizable. Used for the large shared geometry buffers (vertices, indices, per-triangle data) whose total size is unknown upfront.

### CommittedManagedBuffer

Uses a standard committed D3D12 resource — one `ID3D12Resource` backed by a single heap allocation. When capacity is exceeded, the old buffer is pushed to `ToFreeList`, a new buffer is allocated at 2× the size (doubling strategy), and existing data is copied over (via `memcpy` for mapped buffers, `CopyBufferRegion` for GPU-side buffers).

Can be mapped (CPU-visible). Used for upload buffers, smaller allocations, and anything that needs CPU write access.

## Choosing Between Them

| | `ReservedManagedBuffer` | `CommittedManagedBuffer` |
|---|---|---|
| Growth | Map more physical pages, no copy | Allocate new buffer, copy all data |
| CPU access | Not possible | Optional (`isMapped`) |
| Max size | Fixed at construction | Unbounded (doubles as needed) |
| Use when | Large GPU-only buffers with unpredictable growth | Smaller or CPU-writable buffers |
