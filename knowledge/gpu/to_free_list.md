_Last edited: 2026-03-30_

# ToFreeList

`src/rendering/buffer/to_free_list.h/cpp` — deferred deletion for GPU resources.

## The Problem

GPU resources cannot be freed the moment they are no longer needed by CPU code. The GPU may still be executing commands that reference them from the current or previous frames. Releasing a resource while the GPU is reading it is a use-after-free.

## The Pattern

Each frame context holds its own `ToFreeList`. When code wants to destroy a resource, it pushes it onto the current frame's `ToFreeList` instead of freeing it immediately. At the start of the *next* time that same frame context comes around (i.e. `NUM_FRAMES_IN_FLIGHT` frames later), `beginFrame()` waits on that frame's fence — guaranteeing the GPU has finished all work that could have referenced those resources — then calls `freeAll()`.

This means resources live for exactly as many frames as needed: no earlier, no later.

## What It Tracks

Four resource categories, each with its own cleanup logic in `freeAll()`:

- **`resources`** — plain `ComPtr<ID3D12Resource>`, just `Reset()`'d.
- **`mappedResources`** — CPU-mapped resources that need `Unmap()` before `Reset()`.
- **`managedBufferSections`** — allocations inside a `ManagedBuffer` free-list; returned to the allocator via `bufferSection.free()`.
- **`descriptorIdxs`** — descriptor heap slots returned to `sharedDescHeapAlloc`.
- **`instances`** — `Instance*` pointers. On push, the instance is immediately marked `isScheduledForDeletion` and hidden (`setVisible(false)`) so it stops rendering. On `freeAll()`, `instance->reset(true)` fully removes it from the scene and frees its geometry.

## Usage

`toFreeList` is passed into any system that might need to delete resources during an update (e.g. `Terrain::update`, `scene.update`). Code that destroys a resource should always route through it:

```cpp
frameCtx.toFreeList.pushResource(someResource, /*isMapped=*/false);
frameCtx.toFreeList.pushManagedBufferSection(section);
frameCtx.toFreeList.pushInstance(instance);
frameCtx.toFreeList.pushDescriptor(descriptorIdx);
```

Never call `resource.Reset()`, `bufferSection.free()`, or `instance->reset()` directly on a resource that may still be in flight.
