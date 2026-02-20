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

#include "reserved_managed_buffer.h"

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "debug.h"

#include <algorithm>

ReservedManagedBuffer::ReservedManagedBuffer(size_t maxReservedSizeBytes,
                                             D3D12_RESOURCE_STATES initialResourceState,
                                             ManagedBufferOptions options)
    : ManagedBuffer(nullptr /*heapProperties*/, initialResourceState, options),
      maxReservedSizeBytes(maxReservedSizeBytes)
{
    ASSERT(!options.isMapped, "ReservedManagedBuffer cannot be CPU-mapped");
    ASSERT(maxReservedSizeBytes > 0);
    ASSERT(maxReservedSizeBytes % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT == 0,
           "maxReservedSizeBytes must be aligned to D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT");
}

void ReservedManagedBuffer::initializeStorage(size_t sizeBytes)
{
    ASSERT(Renderer::getGraphicsQueue() != nullptr,
           "Renderer graphics queue must be initialised before ReservedManagedBuffer::init");

    D3D12_RESOURCE_DESC resDesc = BASIC_BUFFER_DESC;
    resDesc.Width = static_cast<UINT64>(maxReservedSizeBytes);
    resDesc.Flags = this->options.bufferCreationFlags.resourceFlags;

    CHECK_HRESULT(Renderer::getDevice()->CreateReservedResource(
        &resDesc, this->initialResourceState, nullptr /*pOptimizedClearValue*/, IID_PPV_ARGS(&this->dev_buffer)));

    this->setBufferName();

    const size_t heapSize = mapNewHeap(0 /*virtualStartTile*/, sizeBytes);
    this->mappedCapacityBytes = heapSize;

    // bufferSizeBytes = actual allocated memory
    this->bufferSizeBytes = this->mappedCapacityBytes;

    if (this->options.hasSrvDescriptor)
    {
        // allocated only once
        this->allocSrvDescriptor(nullptr, maxReservedSizeBytes);
    }
}

size_t ReservedManagedBuffer::mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes)
{
    const size_t chunkSize = reservedGrowthChunkBytes;
    size_t heapSize = std::max(chunkSize, minAdditionalBytes);
    heapSize = (heapSize + chunkSize - 1) & ~(chunkSize - 1);

    ASSERT(virtualStartTile * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT + heapSize <= maxReservedSizeBytes,
           "ReservedManagedBuffer ran out of virtual space");

    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = static_cast<UINT64>(heapSize);
    heapDesc.Properties = DEFAULT_HEAP;
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ComPtr<ID3D12Heap> heap;
    CHECK_HRESULT(Renderer::getDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));

    const UINT tileCount = static_cast<UINT>(heapSize / D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
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
                                                     heap.Get(),
                                                     1,
                                                     &rangeFlags,
                                                     &heapOffset,
                                                     &tileCount,
                                                     D3D12_TILE_MAPPING_FLAG_NONE);

    this->heaps.push_back(std::move(heap));
    return heapSize;
}

void ReservedManagedBuffer::ensureCapacity(size_t minCapacityBytes,
                                           bool useBackFreeSection,
                                           ID3D12GraphicsCommandList* cmdList,
                                           ToFreeList& toFreeList)
{
    if (this->mappedCapacityBytes >= minCapacityBytes)
    {
        return;
    }

    const size_t additionalNeeded = minCapacityBytes - this->mappedCapacityBytes;
    const size_t virtualStartTile = this->mappedCapacityBytes / D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    const size_t oldMapped = this->mappedCapacityBytes;

    const size_t heapSize = mapNewHeap(virtualStartTile, additionalNeeded);
    this->mappedCapacityBytes += heapSize;

    this->bufferSizeBytes = this->mappedCapacityBytes;

    this->extendFreelistCapacity(oldMapped, this->mappedCapacityBytes, useBackFreeSection);

    // no need to recreate SRV here since it already covers the entire virtual memory range
}

void ReservedManagedBuffer::onReset()
{
    this->dev_buffer.Reset();

    for (auto& heap : this->heaps)
    {
        heap.Reset();
    }
    this->heaps.clear();

    this->mappedCapacityBytes = 0;
}
