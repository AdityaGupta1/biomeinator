#include "buffer_helper.h"

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"

namespace BufferHelper
{

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         D3D12_HEAP_FLAGS heapFlags,
                                         D3D12_RESOURCE_STATES initialResourceState)
{
    ComPtr<ID3D12Resource> dev_buffer;
    D3D12_RESOURCE_DESC resourceDesc = BASIC_BUFFER_DESC;
    resourceDesc.Width = width;
    Renderer::device->CreateCommittedResource(
        heapProperties, heapFlags, &resourceDesc, initialResourceState, nullptr, IID_PPV_ARGS(&dev_buffer));
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

}  // namespace BufferHelper
