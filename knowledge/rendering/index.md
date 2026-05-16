_Last edited: 2026-05-15_

# Rendering Knowledgebase

High-level render passes, pipeline setup, camera, and NVIDIA integrations.
For D3D12 resource management (buffers, descriptors, AS) see [gpu/](../gpu/index.md).

| Entry | Description |
|---|---|
| [render_loop.md](render_loop.md) | Main game loop, render pass sequence, frame pacing |
| [frame_contexts.md](frame_contexts.md) | Triple buffering, per-frame resources, CPU-GPU pipelining |
| [render_passes.md](render_passes.md) | G-buffer → NRC → path trace → collect → present |
| [pipeline.md](pipeline.md) | DXR pipeline state object, PipelineBuilder, shader tables |
| [param_blocks.md](param_blocks.md) | ParamBlockManager, constant buffer layout and upload |
| [rt_targets.md](rt_targets.md) | RtTarget, UAV/SRV descriptor pairs, G-buffer output textures |
| [camera.md](camera.md) | Spherical coordinate camera, Halton TAA jitter, motion vectors |
| [dlss.md](dlss.md) | NVIDIA Streamline SDK, DLSS-D upsampling integration |
| [nvapi_ser.md](nvapi_ser.md) | NVAPI, Shader Execution Reordering (SER) optimization |
| [gpu_radix_sort.md](gpu_radix_sort.md) | GPU radix sort pass (wraps GPUSorting submodule); tuning + ping-pong invariant |
