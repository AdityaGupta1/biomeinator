// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "buffer_helper.h"

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"

#include "debug.h"

namespace BufferHelper
{

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         BufferCreationFlags optionalFlags)
{
    D3D12_RESOURCE_STATES initialResourceState;
    switch (heapProperties->Type)
    {
        case D3D12_HEAP_TYPE_UPLOAD:
            initialResourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
            break;
        case D3D12_HEAP_TYPE_DEFAULT:
            initialResourceState = D3D12_RESOURCE_STATE_COMMON;
            break;
        case D3D12_HEAP_TYPE_READBACK:
            initialResourceState = D3D12_RESOURCE_STATE_COPY_DEST;
            break;
        default:
            ASSERT(false, "Unsupported heap type");
            return nullptr;
    }

    return createBasicBuffer(width, heapProperties, initialResourceState, optionalFlags);
}

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         D3D12_RESOURCE_STATES initialResourceState,
                                         BufferCreationFlags optionalFlags)
{
    ComPtr<ID3D12Resource> dev_buffer;
    D3D12_RESOURCE_DESC resourceDesc = BASIC_BUFFER_DESC;
    resourceDesc.Width = width;
    resourceDesc.Flags = optionalFlags.resourceFlags;
    CHECK_HRESULT(Renderer::getDevice()->CreateCommittedResource(heapProperties,
                                                                 optionalFlags.heapFlags,
                                                                 &resourceDesc,
                                                                 initialResourceState,
                                                                 nullptr,
                                                                 IID_PPV_ARGS(&dev_buffer)));
    return dev_buffer;
}

void stateTransitionResourceBarrier(ID3D12GraphicsCommandList* cmdList,
                                    ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES stateBefore,
                                    D3D12_RESOURCE_STATES stateAfter)
{
    D3D12_RESOURCE_BARRIER resourceBarrier = {
        .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        .Transition = {
            .pResource = resource,
            .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            .StateBefore = stateBefore,
            .StateAfter = stateAfter,
        },
    };
    cmdList->ResourceBarrier(1, &resourceBarrier);
}

void uavBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    cmdList->ResourceBarrier(1, &barrier);
}

void copyResource(ID3D12GraphicsCommandList* cmdList,
                  ID3D12Resource* destResource,
                  D3D12_RESOURCE_STATES destState,
                  ID3D12Resource* srcResource,
                  D3D12_RESOURCE_STATES srcState)
{
    stateTransitionResourceBarrier(cmdList, srcResource, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stateTransitionResourceBarrier(cmdList, destResource, destState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(destResource, srcResource);

    stateTransitionResourceBarrier(cmdList, destResource, D3D12_RESOURCE_STATE_COPY_DEST, destState);
    stateTransitionResourceBarrier(cmdList, srcResource, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
}

void copyBufferRegion(ID3D12GraphicsCommandList* cmdList,
                      ID3D12Resource* destBuffer,
                      D3D12_RESOURCE_STATES destState,
                      size_t destOffsetBytes,
                      ID3D12Resource* srcBuffer,
                      D3D12_RESOURCE_STATES srcState,
                      size_t srcOffsetBytes,
                      size_t sizeBytes)
{
    stateTransitionResourceBarrier(cmdList, srcBuffer, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    stateTransitionResourceBarrier(cmdList, destBuffer, destState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(destBuffer, destOffsetBytes, srcBuffer, srcOffsetBytes, sizeBytes);

    stateTransitionResourceBarrier(cmdList, destBuffer, D3D12_RESOURCE_STATE_COPY_DEST, destState);
    stateTransitionResourceBarrier(cmdList, srcBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
}

}  // namespace BufferHelper
