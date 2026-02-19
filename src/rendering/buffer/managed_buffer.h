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
    bool hasSrvDescriptor{ false };
    uint32_t srvElementByteSize{ 0 };
    BufferHelper::BufferCreationFlags bufferCreationFlags{};
};

// Abstract base class for GPU buffer management.
//
// Provides:
//   - A two-map freelist allocator (freeByOffset / freeBySize) with block merging.
//   - SRV descriptor allocation and management.
//   - The full public copy / query API.
//
// Concrete subclasses supply three protected virtual hooks:
//   initializeStorage  – create the underlying GPU resource.
//   ensureCapacity     – grow physical backing when the freelist is exhausted.
//   onReset            – release GPU resources at reset time.
//
// Every other method is non-virtual; virtual dispatch is limited to the three hooks
// above plus the destructor.
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
    // For CommittedManagedBuffer: actual physical size of the resource.
    // For ReservedManagedBuffer: bytes of the virtual space currently backed by heaps.
    size_t bufferSizeBytes{ 0 };

    uint32_t srvDescriptorIdx{ ~0u };
    D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptorCpuHandle{};

    // -----------------------------------------------------------------------
    // Freelist internals
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Protected helpers (non-virtual)
    // -----------------------------------------------------------------------

    // Allocate (or reallocate) the SRV descriptor.
    //   explicitSizeBytes == 0  →  use bufferSizeBytes  (for CommittedManagedBuffer).
    //   explicitSizeBytes  > 0  →  use that value        (for ReservedManagedBuffer, which passes
    //                                                      maxReservedSizeBytes so the SRV never
    //                                                      needs recreation).
    void allocSrvDescriptor(ToFreeList* toFreeList, size_t explicitSizeBytes = 0);

    // Update the freelist after capacity has grown from oldSizeBytes to newSizeBytes.
    // If useBackFreeSection is true the trailing free block (if any) is extended in place;
    // otherwise a new free block is inserted at oldSizeBytes.
    void extendFreelistCapacity(size_t oldSizeBytes, size_t newSizeBytes, bool useBackFreeSection);

    void freeSection(ManagedBufferSection section);

    void setBufferName();

    // -----------------------------------------------------------------------
    // Protected virtual hooks (3 + destructor)
    // -----------------------------------------------------------------------

    // Create (or recreate) the GPU resource for the given size.
    // Must set dev_buffer and bufferSizeBytes before returning.
    virtual void initializeStorage(size_t sizeBytes) = 0;

    // Grow physical backing so that at least minCapacityBytes are accessible.
    // Must update bufferSizeBytes and call extendFreelistCapacity before returning.
    virtual void ensureCapacity(size_t minCapacityBytes,
                                bool useBackFreeSection,
                                ID3D12GraphicsCommandList* cmdList,
                                ToFreeList& toFreeList) = 0;

    // Release GPU resources owned by this object.
    // Default implementation: dev_buffer.Reset().
    // Subclasses that own additional resources (e.g. heaps) override this.
    virtual void onReset();

    // -----------------------------------------------------------------------
    // Constructor (protected – base is abstract)
    // -----------------------------------------------------------------------
    ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                  const D3D12_RESOURCE_STATES initialResourceState,
                  const ManagedBufferOptions options);

    // CPU map/unmap – protected so only the base class and subclasses can call them.
    // CommittedManagedBuffer calls map() inside ensureCapacity for CPU-mapped buffers.
    // ReservedManagedBuffer never calls them (asserted at construction via isMapped==false).
    void map();
    void unmap();

public:
    virtual ~ManagedBuffer() = default;

    void init(size_t sizeBytes);

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
    bool hasValidSrvDescriptor() const;
    uint32_t getSrvDescriptorIdx() const;
    size_t getSizeBytes() const;

    void setName(const std::wstring& name);
};
