// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

class DescriptorHeapAllocator
{
private:
    ID3D12DescriptorHeap* heapPtr{ nullptr };
    D3D12_CPU_DESCRIPTOR_HANDLE heapStartCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE heapStartGpu{};
    uint32_t heapHandleIncrement{ ~0u };
    std::vector<uint32_t> freeIdxs{};

public:
    void init(ID3D12Device* device, ID3D12DescriptorHeap* heapPtr);

    uint32_t alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle);
    uint32_t alloc(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);

    void free(uint32_t idx);
    void free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
};
