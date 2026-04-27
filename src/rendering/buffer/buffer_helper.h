// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"

#include <vector>

namespace BufferHelper
{

struct TransitionBatch
{
    void add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void addUavBarrier(ID3D12Resource* resource = nullptr);
    void submit(ID3D12GraphicsCommandList* cmdList);

private:
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
};

struct BufferCreationFlags
{
    D3D12_HEAP_FLAGS heapFlags{ D3D12_HEAP_FLAG_NONE };
    D3D12_RESOURCE_FLAGS resourceFlags{ D3D12_RESOURCE_FLAG_NONE };
};

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         BufferCreationFlags optionalFlags = {});

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         D3D12_RESOURCE_STATES initialResourceState,
                                         BufferCreationFlags optionalFlags = {});

void stateTransitionResourceBarrier(ID3D12GraphicsCommandList* cmdList,
                                    ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES stateBefore,
                                    D3D12_RESOURCE_STATES stateAfter);

void uavBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource);

void copyResource(ID3D12GraphicsCommandList* cmdList,
                  ID3D12Resource* destResource,
                  D3D12_RESOURCE_STATES destState,
                  ID3D12Resource* srcResource,
                  D3D12_RESOURCE_STATES srcState);

void copyBufferRegion(ID3D12GraphicsCommandList* cmdList,
                      ID3D12Resource* destBuffer,
                      D3D12_RESOURCE_STATES destState,
                      size_t destOffsetBytes,
                      ID3D12Resource* srcBuffer,
                      D3D12_RESOURCE_STATES srcState,
                      size_t srcOffsetBytes,
                      size_t sizeBytes);

} // namespace BufferHelper
