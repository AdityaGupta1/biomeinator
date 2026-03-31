_Last edited: 2026-03-30_

# GPU Resource Management Knowledgebase

D3D12 resource abstractions shared across the rendering and scene subsystems.

| Entry | Description |
|---|---|
| [managed_buffer.md](managed_buffer.md) | ManagedBuffer base class, free-list allocator, batch copy |
| [reserved_managed_buffer.md](reserved_managed_buffer.md) | Virtual memory strategy, D3D12 placed resources, 4 GB virtual alloc |
| [committed_managed_buffer.md](committed_managed_buffer.md) | Traditional heap commitment strategy for smaller/upload buffers |
| [mapped_array.md](mapped_array.md) | CPU-visible typed arrays, dirty range tracking, partial GPU uploads |
| [descriptor_heap.md](descriptor_heap.md) | DescriptorHeapAllocator, CPU/GPU handle pairs, free index pool |
| [acceleration_structures.md](acceleration_structures.md) | AcsHelper, BLAS/TLAS construction, instance descriptors |
| [to_free_list.md](to_free_list.md) | Deferred deletion pattern, frame-boundary resource cleanup |
| [fence.md](fence.md) | GPU/CPU synchronization fence |
