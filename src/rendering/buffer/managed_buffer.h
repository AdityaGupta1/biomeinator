// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/free_range_allocator.h"
#include "rendering/buffer/gpu_memory_reporter.h"
#include "rendering/dxr_includes.h"
#include "util/util.h"

class ToFreeList;

class ManagedBuffer;

struct ManagedBufferSection
{
private:
    ManagedBuffer* buffer;

public:
    size_t offsetBytes;
    size_t sizeBytes;

    ManagedBufferSection(ManagedBuffer* buffer, size_t offsetBytes, size_t sizeBytes);
    ManagedBufferSection();

    ManagedBuffer* getBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS getGpuVirtualAddress() const;

    inline bool isValid() const
    {
        return this->sizeBytes > 0;
    }

    void free();
};

struct ManagedBufferOptions
{
    bool isResizable{ false };
    bool isMapped{ false };
    size_t alignmentBytes{ 0 }; // if nonzero, requested section sizes are rounded up to a multiple of this
    BufferHelper::BufferCreationFlags bufferCreationFlags{};
};

class ManagedBuffer : public GpuMemoryReporter
{
    friend class ManagedBufferSection;
    friend class ToFreeList;

protected:
    std::wstring name{ L"ManagedBuffer" };

    const D3D12_HEAP_PROPERTIES* heapProperties;
    const D3D12_RESOURCE_STATES initialResourceState;

    const ManagedBufferOptions options;

    void* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };
    size_t bufferSizeBytes{
        0
    }; // actual physical allocated memory (i.e. not virtual memory in case of ReservedManagedBuffer)
    FreeRangeAllocator freeRanges;

    bool batchCopyActive{ false };

    void freeSection(ManagedBufferSection section);

    void setBufferName();

    virtual void initializeStorage(ToFreeList* toFreeList, size_t sizeBytes) = 0;

    virtual void ensureCapacity(ID3D12GraphicsCommandList* cmdList,
                                ToFreeList& toFreeList,
                                size_t minCapacityBytes) = 0;

    virtual void onReset() = 0;

    ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                  const D3D12_RESOURCE_STATES initialResourceState,
                  const ManagedBufferOptions options);

    void map();
    void unmap();

public:
    virtual ~ManagedBuffer() = default;

    void init(size_t sizeBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);

    void reset();

    ManagedBufferSection findFreeSection(ID3D12GraphicsCommandList* cmdList, ToFreeList* toFreeList, size_t sizeBytes);

    ManagedBufferSection copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                            ToFreeList& toFreeList,
                                            const void* host_srcBuffer,
                                            size_t sizeBytes);
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
                                              size_t srcSizeBytes,
                                              size_t srcOffsetBytes = 0);
    ManagedBufferSection copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                               ToFreeList& toFreeList,
                                               const ManagedBuffer& srcBuffer,
                                               ManagedBufferSection srcBufferSection);

    void beginBatchCopy(ID3D12GraphicsCommandList* cmdList);
    void endBatchCopy(ID3D12GraphicsCommandList* cmdList);

    ID3D12Resource* getBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS getGpuVirtualAddress() const;
    size_t getSizeBytes() const;
    size_t getFreeBytes() const;

    GpuMemoryEntry reportGpuMemory() const override;

    void setName(const std::wstring& name);
};
