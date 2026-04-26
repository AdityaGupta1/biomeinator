_Last edited: 2026-04-26_

# Render Targets

`RtTarget` wraps a 2D texture with optional UAV and SRV descriptors in the shared descriptor
heap. Used for all intermediate render targets (path tracing output, DLSS guide buffers,
debug view, etc.).

## isFullSize

Targets marked `isFullSize` are created at viewport/display resolution rather than render
resolution. `dlssOutputTarget` and `debugTarget` use this — most targets are at render
resolution (which may be smaller than viewport when DLSS upscaling is active).

## State Tracking Gotcha

`transitionToState()` tracks resource state in CPU-side `targetResourceState`. The comment
in the source notes uncertainty about whether this works correctly across frame boundaries
with D3D12 resource promotion/decay. Currently works because all RT targets start each frame
with an explicit transition to `UNORDERED_ACCESS`.

## Reset Doesn't Use ToFreeList

`reset()` immediately releases the resource and descriptor heap slots. This is safe because
it's only called during `resize()`, which calls `flush()` first to drain the GPU.
