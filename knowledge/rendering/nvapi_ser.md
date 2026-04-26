_Last edited: 2026-04-26_

# NVAPI and Shader Execution Reordering

## SER (Shader Execution Reordering)

SER lets the GPU reorder ray tracing threads by a coherence hint to improve warp occupancy.
Enabled via NVAPI at init if the GPU supports `NV_EXTN_OP_HIT_OBJECT_REORDER_THREAD`.

On the shader side, the path tracer calls `NvReorderThread()` with a coherence hint at each
bounce. Hints differentiate: first-bounce hits, passthrough (delta transmissive), and
scattering (non-delta) surfaces. This groups threads that will follow similar code paths
together.

## NVAPI Init Quirk

`initNvapi()` calls `NvAPI_Initialize()` then immediately `NvAPI_Unload()` before calling
`NvAPI_D3D12_IsNvShaderExtnOpCodeSupported` and `NvAPI_D3D12_SetNvShaderExtnSlotSpace`. This
pattern comes from [NVIDIA's SER integration guide](https://developer.nvidia.com/blog/improve-shader-performance-and-in-game-frame-rates-with-shader-execution-reordering/)
which shows it without explanation. The D3D12 NVAPI functions apparently work after unload.
