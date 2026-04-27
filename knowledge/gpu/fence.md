_Last edited: 2026-04-26_

# Fence

`src/rendering/renderer/fence.h/cpp` — thin wrapper around `ID3D12Fence` for CPU/GPU
synchronization.

## Role

Single fence instance used by the renderer. Each frame, after submitting the command list,
`signal()` returns a fence value stored in the frame context. At `beginFrame()`, `waitFor()`
blocks until the GPU completes that frame's work, ensuring the frame context is safe to
reuse. Flushing (signal + immediate wait) is done by the renderer, not by the Fence class
itself.
