#pragma once

#include "rendering/dxr_includes.h"
#include "util/util.h"

#include <list>

class ToFreeList;

class ManagedBuffer;

struct ManagedBufferSection
{
private:
    ManagedBuffer* buffer;

public:
    uint32_t offsetBytes;
    uint32_t sizeBytes;

    ManagedBufferSection(ManagedBuffer* buffer, uint32_t offsetBytes, uint32_t sizeBytes);
    ManagedBufferSection();

    ManagedBuffer* getManagedBuffer() const;
};

class ManagedBuffer
{
    friend class ToFreeList;

private:
    const D3D12_HEAP_PROPERTIES* heapProperties;
    const D3D12_HEAP_FLAGS heapFlags;
    const D3D12_RESOURCE_STATES initialResourceState;

    const bool isResizable;
    const bool isMapped;

    void* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    uint32_t bufferSizeBytes{ 0 };

    std::list<ManagedBufferSection> freeSectionList;

    ManagedBufferSection findFreeSection(ID3D12GraphicsCommandList* cmdList,
                                         ToFreeList& toFreeList,
                                         uint32_t sizeBytes);
    // resize() works only for non-mapped buffers
    void resize(ID3D12GraphicsCommandList* cmdList,
                ToFreeList& toFreeList,
                uint32_t newSizeBytes,
                bool useBackFreeSection);

    void freeSection(ManagedBufferSection section);

public:
    ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                  const D3D12_HEAP_FLAGS heapFlags,
                  const D3D12_RESOURCE_STATES initialResourceState,
                  const bool isResizable,
                  const bool isMapped);

    void init(uint32_t sizeBytes);

    void map();
    void unmap();

    ManagedBufferSection copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                            ToFreeList& toFreeList,
                                            const void* host_srcBuffer,
                                            uint32_t sizeBytes);
    template<typename T>
    inline ManagedBufferSection copyFromHostVector(ID3D12GraphicsCommandList* cmdList,
                                                   ToFreeList& toFreeList,
                                                   const std::vector<T>& host_srcVector)
    {
        return this->copyFromHostBuffer(cmdList,
                                        toFreeList,
                                        static_cast<const void*>(host_srcVector.data()),
                                        Util::getVectorSizeBytes(host_srcVector));
    }

    ManagedBufferSection copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                              ToFreeList& toFreeList,
                                              ID3D12Resource* dev_srcBuffer,
                                              uint32_t sizeBytes,
                                              uint32_t offsetBytes = 0);
    ManagedBufferSection copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                               ToFreeList& toFreeList,
                                               const ManagedBuffer& dev_srcBuffer,
                                               ManagedBufferSection srcBufferSection);

    ID3D12Resource* getManagedBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS getBufferGpuAddress() const;
    uint32_t getSizeBytes() const;
};
