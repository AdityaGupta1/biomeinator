/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
