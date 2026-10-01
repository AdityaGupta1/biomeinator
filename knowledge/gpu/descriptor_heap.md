_Last edited: 2026-09-22_

# Descriptor Heap Allocator

`src/rendering/buffer/descriptor_heap_allocator.h/cpp` — simple free-list allocator for
slots in a D3D12 descriptor heap.

## Single Shared Heap

One `DescriptorHeapAllocator` wraps the single shared `CBV_SRV_UAV` descriptor heap used by
the entire renderer. All bindless resource access goes through this heap — textures, RT
targets, and SRVs/UAVs all get slots here. The returned index is what shaders use in
`ResourceDescriptorHeap[idx]`.

## No Fragmentation Concern

Unlike `ManagedBuffer`'s free-list, this allocator doesn't need to worry about contiguous
ranges — each descriptor slot is independent. Free slots are pushed/popped from a vector
acting as a stack (LIFO order).

The index lifecycle and CPU/GPU handle arithmetic live in `DescriptorIndexAllocator`, while
`DescriptorHeapAllocator` owns the actual D3D heap. Separating those roles makes exhaustion,
LIFO reuse, and handle round-tripping CPU-testable. The index allocator records which slots
are live, so a foreign handle, mismatched CPU/GPU pair, or duplicate release is rejected at
the heap boundary rather than adding the same slot to the free stack twice.
