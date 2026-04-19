// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "descriptor_heap_allocator.h"

#include "debug.h"

void DescriptorHeapAllocator::init(ID3D12Device* device, ID3D12DescriptorHeap* heapPtr)
{
    this->heapPtr = heapPtr;
    const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = heapPtr->GetDesc();

    this->heapHandleIncrement = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    this->heapStartCpu = heapPtr->GetCPUDescriptorHandleForHeapStart();
    this->heapStartGpu = heapPtr->GetGPUDescriptorHandleForHeapStart();

    const uint32_t numDescriptors = heapDesc.NumDescriptors;
    this->freeIdxs.reserve(numDescriptors);
    for (int idx = numDescriptors - 1; idx >= 0; --idx)
    {
        this->freeIdxs.push_back(idx);
    }
}

uint32_t DescriptorHeapAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle)
{
    return alloc(outCpuHandle, nullptr);
}

uint32_t DescriptorHeapAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    ASSERT(this->freeIdxs.size() > 0);
    const uint32_t idx = this->freeIdxs.back();
    this->freeIdxs.pop_back();
    outCpuHandle->ptr = this->heapStartCpu.ptr + (idx * this->heapHandleIncrement);
    if (outGpuHandle != nullptr)
    {
        outGpuHandle->ptr = this->heapStartGpu.ptr + (idx * this->heapHandleIncrement);
    }
    return idx;
}

void DescriptorHeapAllocator::free(uint32_t idx)
{
    this->freeIdxs.push_back(idx);
}

void DescriptorHeapAllocator::free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    const uint32_t cpuIdx = (cpuHandle.ptr - this->heapStartCpu.ptr) / this->heapHandleIncrement;
    const uint32_t gpuIdx = (gpuHandle.ptr - this->heapStartGpu.ptr) / this->heapHandleIncrement;
    ASSERT(cpuIdx == gpuIdx);
    this->free(cpuIdx);
}
