// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "reserved_managed_buffer.h"

#include "debug.h"
#include "logger.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "util/math.h"

#include <algorithm>

inline constexpr size_t reservedGrowthChunkBytes = 64ull * 1024 * 1024; // 64 MB

static ComPtr<ID3D12Heap> createHeap(const size_t sizeBytes)
{
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = static_cast<UINT64>(sizeBytes);
    heapDesc.Properties = DEFAULT_HEAP;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    ComPtr<ID3D12Heap> heap;
    CHECK_HRESULT(Renderer::getDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    return heap;
}

static_assert(reservedGrowthChunkBytes % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT == 0,
              "reservedGrowthChunkBytes must be a multiple of D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT");

ReservedManagedBuffer::ReservedManagedBuffer(size_t maxReservedSizeBytes,
                                             D3D12_RESOURCE_STATES initialResourceState,
                                             ManagedBufferOptions options)
    : ManagedBuffer(nullptr /*heapProperties*/, initialResourceState, options),
      maxReservedSizeBytes(maxReservedSizeBytes)
{
    ASSERT(options.isResizable, "ReservedManagedBuffer must be resizable");
    ASSERT(!options.isMapped, "ReservedManagedBuffer cannot be mapped");
    ASSERT(maxReservedSizeBytes > 0);
    ASSERT(maxReservedSizeBytes % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT == 0,
           "maxReservedSizeBytes must be aligned to D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT");
}

void ReservedManagedBuffer::initializeStorage(ToFreeList* toFreeList, size_t sizeBytes)
{
    ASSERT(Renderer::getGraphicsQueue() != nullptr,
           "Renderer graphics queue must be initialised before ReservedManagedBuffer::init");

    D3D12_RESOURCE_DESC resDesc = BASIC_BUFFER_DESC;
    resDesc.Width = static_cast<UINT64>(maxReservedSizeBytes);
    resDesc.Flags = this->options.bufferCreationFlags.resourceFlags;

    CHECK_HRESULT(Renderer::getDevice()->CreateReservedResource(
        &resDesc, this->initialResourceState, nullptr /*pOptimizedClearValue*/, IID_PPV_ARGS(&this->dev_buffer)));

    const size_t heapSize = mapNewHeap(0 /*virtualStartTile*/, sizeBytes, false /*prefetchNext*/);
    this->bufferSizeBytes = heapSize;

    this->setBufferName();
}

size_t ReservedManagedBuffer::mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes, bool prefetchNext)
{
    ASSERT(minAdditionalBytes > 0);
    const size_t newHeapSize = MathUtil::roundUpToPow2(minAdditionalBytes, reservedGrowthChunkBytes);

    const bool fitsVirtualSpace =
        virtualStartTile * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT + newHeapSize <= maxReservedSizeBytes;
    if (!fitsVirtualSpace)
    {
        Logger::logError("ReservedManagedBuffer %ls ran out of virtual space", this->name.c_str());
    }
    ASSERT(fitsVirtualSpace);

    ComPtr<ID3D12Heap> newHeap;
    if (this->prefetchedHeap.valid())
    {
        newHeap = this->prefetchedHeap.get();
    }
    if (newHeap == nullptr || this->prefetchedHeapSizeBytes != newHeapSize)
    {
        newHeap = createHeap(newHeapSize);
    }
    if (prefetchNext)
    {
        this->prefetchedHeapSizeBytes = reservedGrowthChunkBytes;
        this->prefetchedHeap = std::async(std::launch::async, createHeap, reservedGrowthChunkBytes);
    }

    const UINT tileCount = static_cast<UINT>(newHeapSize / D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    const UINT heapOffset = 0;
    D3D12_TILE_RANGE_FLAGS rangeFlags = D3D12_TILE_RANGE_FLAG_NONE;

    D3D12_TILED_RESOURCE_COORDINATE startCoord = {};
    startCoord.X = static_cast<UINT>(virtualStartTile);

    D3D12_TILE_REGION_SIZE regionSize = {};
    regionSize.NumTiles = tileCount;
    regionSize.UseBox = FALSE;

    Renderer::getGraphicsQueue()->UpdateTileMappings(this->dev_buffer.Get(),
                                                     1,
                                                     &startCoord,
                                                     &regionSize,
                                                     newHeap.Get(),
                                                     1,
                                                     &rangeFlags,
                                                     &heapOffset,
                                                     &tileCount,
                                                     D3D12_TILE_MAPPING_FLAG_NONE);

    this->heaps.push_back(std::move(newHeap));
    return newHeapSize;
}

void ReservedManagedBuffer::ensureCapacity(ID3D12GraphicsCommandList* cmdList,
                                            ToFreeList& toFreeList,
                                            size_t minCapacityBytes,
                                            bool useBackFreeSection)
{
    if (this->bufferSizeBytes >= minCapacityBytes)
    {
        return;
    }

    const size_t additionalNeeded = minCapacityBytes - this->bufferSizeBytes;
    const size_t virtualStartTile = this->bufferSizeBytes / D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    const size_t oldBufferSizeBytes = this->bufferSizeBytes;

    const size_t heapSize = mapNewHeap(virtualStartTile, additionalNeeded, true /*prefetchNext*/);
    this->bufferSizeBytes += heapSize;

    this->extendFreelistCapacity(oldBufferSizeBytes, this->bufferSizeBytes, useBackFreeSection);
}

void ReservedManagedBuffer::onReset()
{
    if (this->prefetchedHeap.valid())
    {
        this->prefetchedHeap.get();
    }
    this->dev_buffer.Reset();
    this->bufferSizeBytes = 0;

    for (auto& heap : this->heaps)
    {
        heap.Reset();
    }
    this->heaps.clear();
}
