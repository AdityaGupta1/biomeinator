/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/buffer/buffer_helper.h"
#include "util/util.h"

#include <map>

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

    void free() const;
};

struct ManagedBufferOptions
{
    bool isResizable{ false };
    bool isMapped{ false };
    BufferHelper::BufferCreationFlags bufferCreationFlags{};
};

class ManagedBuffer
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
    size_t bufferSizeBytes{ 0 }; // actual physical allocated memory (i.e. not virtual memory in case of ReservedManagedBuffer)

    struct FreeNode;
    using OffsetMap = std::map<size_t, FreeNode>;
    using OffsetIter = OffsetMap::iterator;
    using SizeMap = std::multimap<size_t, OffsetIter>;
    using SizeIter = SizeMap::iterator;
    struct FreeNode
    {
        size_t sizeBytes;
        SizeIter sizeIter;
    };
    OffsetMap freeByOffset;
    SizeMap freeBySize;

    void insertFreeNode(size_t offsetBytes, size_t sizeBytes);
    void eraseFreeNode(OffsetIter offsetIter);

    void extendFreelistCapacity(size_t oldSizeBytes, size_t newSizeBytes, bool useBackFreeSection);

    void freeSection(ManagedBufferSection section);

    void setBufferName();

    virtual void initializeStorage(ToFreeList* toFreeList, size_t sizeBytes) = 0;

    virtual void ensureCapacity(ID3D12GraphicsCommandList* cmdList,
                                ToFreeList& toFreeList,
                                size_t minCapacityBytes,
                                bool useBackFreeSection) = 0;

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

    ManagedBufferSection findFreeSection(ID3D12GraphicsCommandList* cmdList,
                                         ToFreeList* toFreeList,
                                         size_t sizeBytes);

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

    ID3D12Resource* getBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS getGpuVirtualAddress() const;
    size_t getSizeBytes() const;

    void setName(const std::wstring& name);
};
