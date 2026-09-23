// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "descriptor_heap_allocator.h"

#include "debug.h"

void DescriptorHeapAllocator::init(ID3D12Device* device, ID3D12DescriptorHeap* heapPtr)
{
    const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = heapPtr->GetDesc();
    const D3D12_CPU_DESCRIPTOR_HANDLE heapStartCpu = heapPtr->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE heapStartGpu = heapPtr->GetGPUDescriptorHandleForHeapStart();
    const uint32_t heapHandleIncrement = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    this->indexAllocator.reset(heapDesc.NumDescriptors, heapStartCpu.ptr, heapStartGpu.ptr, heapHandleIncrement);
}

uint32_t DescriptorHeapAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle)
{
    return alloc(outCpuHandle, nullptr);
}

uint32_t DescriptorHeapAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    ASSERT(outCpuHandle != nullptr);
    if (outCpuHandle == nullptr)
    {
        return DescriptorIndexAllocation::INVALID_INDEX;
    }

    const std::optional<DescriptorIndexAllocation> allocation = this->indexAllocator.allocate();
    ASSERT(allocation.has_value(), "Descriptor heap exhausted");
    if (!allocation)
    {
        *outCpuHandle = {};
        if (outGpuHandle != nullptr)
        {
            *outGpuHandle = {};
        }
        return DescriptorIndexAllocation::INVALID_INDEX;
    }

    outCpuHandle->ptr = allocation->cpuHandle;
    if (outGpuHandle != nullptr)
    {
        outGpuHandle->ptr = allocation->gpuHandle;
    }
    return allocation->index;
}

void DescriptorHeapAllocator::free(uint32_t idx)
{
    const bool didRelease = this->indexAllocator.release(idx);
    ASSERT(didRelease, "Attempted to free an invalid or already-free descriptor index");
}

void DescriptorHeapAllocator::free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    const bool didRelease = this->indexAllocator.release(cpuHandle.ptr, gpuHandle.ptr);
    ASSERT(didRelease, "Attempted to free invalid or mismatched descriptor handles");
}
