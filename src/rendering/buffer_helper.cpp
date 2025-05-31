#include "buffer_helper.h"

#include "dxr_common.h"
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

}  // namespace BufferHelper
