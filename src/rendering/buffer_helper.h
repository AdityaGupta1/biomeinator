#pragma once

#include "dxr_includes.h"

namespace BufferHelper
{

ComPtr<ID3D12Resource> createBasicBuffer(uint64_t width,
                                         const D3D12_HEAP_PROPERTIES* heapProperties,
                                         D3D12_HEAP_FLAGS heapFlags,
                                         D3D12_RESOURCE_STATES initialResourceState);

void stateTransitionResourceBarrier(ID3D12GraphicsCommandList* cmdList,
                                    ID3D12Resource* resource,
                                    D3D12_RESOURCE_STATES stateBefore,
                                    D3D12_RESOURCE_STATES stateAfter);

void uavBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource);

} // namespace BufferHelper
