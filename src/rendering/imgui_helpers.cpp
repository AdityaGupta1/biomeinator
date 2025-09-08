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

// This file is largely based on https://github.com/ocornut/imgui/blob/master/examples/example_win32_directx12/main.cpp

#include "imgui_helpers.h"

#include "debug.h"

void ImguiDescriptorHeapAllocator::init(
    ID3D12Device* device, ID3D12DescriptorHeap* heapPtr, uint32_t size, uint32_t offset)
{
    this->heapPtr = heapPtr;
    const D3D12_DESCRIPTOR_HEAP_DESC heapDesc = heapPtr->GetDesc();

    this->heapHandleIncrement = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    this->heapStartCpu = heapPtr->GetCPUDescriptorHandleForHeapStart();
    this->heapStartGpu = heapPtr->GetGPUDescriptorHandleForHeapStart();

    const uint32_t heapStartOffset = offset * this->heapHandleIncrement;
    this->heapStartCpu.ptr += heapStartOffset;
    this->heapStartGpu.ptr += heapStartOffset;

    this->freeIdxs.reserve(size);
    for (int idx = 0; idx < size; ++idx)
    {
        this->freeIdxs.push_back(idx);
    }
}

void ImguiDescriptorHeapAllocator::alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuDescHandle,
                                         D3D12_GPU_DESCRIPTOR_HANDLE* outGpuDescHandle)
{
    ASSERT(this->freeIdxs.size() > 0);
    const uint32_t idx = this->freeIdxs.back();
    this->freeIdxs.pop_back();
    outCpuDescHandle->ptr = this->heapStartCpu.ptr + (idx * this->heapHandleIncrement);
    outGpuDescHandle->ptr = this->heapStartGpu.ptr + (idx * this->heapHandleIncrement);
}

void ImguiDescriptorHeapAllocator::free(D3D12_CPU_DESCRIPTOR_HANDLE cpuDescHandle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE gpuDescHandle)
{
    const uint32_t cpuIdx = (cpuDescHandle.ptr - this->heapStartCpu.ptr) / this->heapHandleIncrement;
    const uint32_t gpuIdx = (gpuDescHandle.ptr - this->heapStartGpu.ptr) / this->heapHandleIncrement;
    ASSERT(cpuIdx == gpuIdx);
    this->freeIdxs.push_back(cpuIdx);
}
