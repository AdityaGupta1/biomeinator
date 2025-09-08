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

#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

class DescriptorHeapAllocator
{
private:
    ID3D12DescriptorHeap* heapPtr{ nullptr };
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE heapStartGpu;
    uint32_t heapHandleIncrement;
    std::vector<uint32_t> freeIdxs;

public:
    void init(ID3D12Device* device, ID3D12DescriptorHeap* heapPtr);

    uint32_t alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuDescHandle);

    void free(uint32_t idx);
};
