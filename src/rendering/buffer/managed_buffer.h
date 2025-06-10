#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/to_free_list.h"
#include "util/util.h"

#include <list>

struct ManagedBufferSection
{
    uint32_t offsetBytes{ 0 };
    uint32_t sizeBytes{ 0 };
};

class ManagedBuffer
{
private:
    const D3D12_HEAP_PROPERTIES* heapProperties;
    const D3D12_HEAP_FLAGS heapFlags;
    const D3D12_RESOURCE_STATES initialResourceState;

    const bool isMapped;

    void* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint32_t bufferSizeBytes{ 0 };

    std::list<ManagedBufferSection> freeSectionList;

    ManagedBufferSection findFreeSection(uint32_t sizeBytes);

public:
    ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                  const D3D12_HEAP_FLAGS heapFlags,
                  const D3D12_RESOURCE_STATES initialResourceState,
                  const bool isMapped);

    void init(uint32_t sizeBytes);

    void map();
    void unmap();

    ManagedBufferSection copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                            const void* host_srcBuffer,
                                            uint32_t sizeBytes);
    template<typename T>
    inline ManagedBufferSection copyFromHostVector(ID3D12GraphicsCommandList* cmdList,
                                                   const std::vector<T>& host_srcVector)
    {
        return this->copyFromHostBuffer(
            cmdList, static_cast<const void*>(host_srcVector.data()), Util::getVectorSizeBytes(host_srcVector));
    }

    ManagedBufferSection copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                              ID3D12Resource* dev_srcBuffer,
                                              uint32_t sizeBytes,
                                              uint32_t offsetBytes = 0);
    ManagedBufferSection copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                               const ManagedBuffer& dev_srcBuffer,
                                               ManagedBufferSection srcBufferSection);

    void freeSection(ManagedBufferSection section);

    void queueFreeBuffer(ToFreeList& toFreeList);

    ID3D12Resource* getBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS getBufferGpuAddress() const;
    uint32_t getSizeBytes() const;
};
