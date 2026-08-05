_Last edited: 2026-08-04_

# ToFreeList

`src/rendering/buffer/to_free_list.h/cpp` — deferred deletion for GPU resources.

## The Problem

GPU resources cannot be freed the moment they are no longer needed by CPU code. The GPU may still be executing commands that reference them from the current or previous frames. Releasing a resource while the GPU is reading it is a use-after-free.

## The Pattern

Each frame context holds its own `ToFreeList`. When code wants to destroy a resource, it pushes it onto the current frame's `ToFreeList` instead of freeing it immediately. At the start of the *next* time that same frame context comes around (i.e. `NUM_FRAMES_IN_FLIGHT` frames later), `beginFrame()` waits on that frame's fence — guaranteeing the GPU has finished all work that could have referenced those resources — then calls `freeAll()`.

This means resources live for exactly as many frames as needed: no earlier, no later.

## What It Tracks

Plain `ID3D12Resource`s, `ManagedBuffer` sections, descriptor heap slots, and
`Instance*`s. Non-obvious points:

- Mapped resources need no special handling: per the D3D12 spec, `Unmap` never
  needs to be called — releasing the last reference cleans up the mapping, as
  long as the mapped pointer is never touched afterwards.
- Pushed instances are immediately hidden and marked `isScheduledForDeletion`
  so they stop rendering during the deferred-free window.

## Usage

`toFreeList` is passed into any system that might need to delete resources during an update (e.g. `Terrain::update`, `scene.update`). Code that destroys a resource should always route through it:

```cpp
frameCtx.toFreeList.pushResource(someResource);
frameCtx.toFreeList.pushManagedBufferSection(section);
frameCtx.toFreeList.pushInstance(instance);
frameCtx.toFreeList.pushDescriptor(descriptorIdx);
```

Never call `resource.Reset()`, `bufferSection.free()`, or `instance->reset()` directly on a resource that may still be in flight.
