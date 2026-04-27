_Last edited: 2026-04-26_

# Frame Contexts

`FrameContext` in `renderer_internal.h` — one per frame-in-flight (3). Each owns a command
allocator, a `ToFreeList`, and a `ParamBlockManager`.

## Why Per-Frame

CPU writes params and queues GPU resource deletions while previous frames are still in
flight. Tripling these resources avoids CPU/GPU races without explicit synchronization per
resource.

## ToFreeList Lifetime

`toFreeList.freeAll()` runs at `beginFrame()` after the fence confirms the GPU finished that
frame context's work. Anything pushed to `toFreeList` during frame N is guaranteed to survive
until the GPU finishes frame N. This is how buffer sections and temp upload resources are
safely deferred-deleted.

## ParamBlockManager Per-Frame

Each frame context has its own `ParamBlockManager` so the CPU can write next frame's params
while the GPU reads the current frame's. The param buffer is a single persistently-mapped
upload buffer — no copy step needed.
