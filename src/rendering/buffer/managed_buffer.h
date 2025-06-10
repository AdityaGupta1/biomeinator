#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/to_free_list.h"

#include <list>

struct ManagedBufferSection
{
    uint32_t offsetBytes;
    uint32_t sizeBytes;
};

class ManagedBuffer
{
private:
    const D3D12_HEAP_PROPERTIES* heapProperties;
    const D3D12_HEAP_FLAGS heapFlags;
    const D3D12_RESOURCE_STATES initialResourceState;

    void* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint32_t bufferSize{ 0 };

    std::list<ManagedBufferSection> freeList;

    ManagedBufferSection findFreeSection(uint32_t sizeBytes);

public:
    ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                  const D3D12_HEAP_FLAGS heapFlags,
                  const D3D12_RESOURCE_STATES initialResourceState);

    void init(uint32_t sizeBytes);
    void resize(uint32_t sizeBytes, ToFreeList& toFreeList);

    void map();
    void unmap();

    ManagedBufferSection copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                            void* host_srcBuffer,
                                            uint32_t sizeBytes);
    ManagedBufferSection copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                              ID3D12Resource* dev_srcBuffer,
                                              uint32_t sizeBytes);

    void freeSection(ManagedBufferSection section);

    ID3D12Resource* getBuffer();
};
