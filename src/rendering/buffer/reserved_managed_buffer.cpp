/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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

#include "debug.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "util/math.h"

#include <algorithm>

inline constexpr size_t reservedGrowthChunkBytes = 32ull * 1024 * 1024; // 32 MB

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

    const size_t heapSize = mapNewHeap(0 /*virtualStartTile*/, sizeBytes);
    this->bufferSizeBytes = heapSize;

    this->setBufferName();
}

size_t ReservedManagedBuffer::mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes)
{
    ASSERT(minAdditionalBytes > 0);
    const size_t newHeapSize = MathUtil::roundUp(minAdditionalBytes, reservedGrowthChunkBytes);

    ASSERT(virtualStartTile * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT + newHeapSize <= maxReservedSizeBytes,
           "ReservedManagedBuffer ran out of virtual space");

    D3D12_HEAP_DESC newHeapDesc = {};
    newHeapDesc.SizeInBytes = static_cast<UINT64>(newHeapSize);
    newHeapDesc.Properties = DEFAULT_HEAP;
    newHeapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    newHeapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ComPtr<ID3D12Heap> newHeap;
    CHECK_HRESULT(Renderer::getDevice()->CreateHeap(&newHeapDesc, IID_PPV_ARGS(&newHeap)));

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

    const size_t heapSize = mapNewHeap(virtualStartTile, additionalNeeded);
    this->bufferSizeBytes += heapSize;

    this->extendFreelistCapacity(oldBufferSizeBytes, this->bufferSizeBytes, useBackFreeSection);
}

void ReservedManagedBuffer::onReset()
{
    this->dev_buffer.Reset();
    this->bufferSizeBytes = 0;

    for (auto& heap : this->heaps)
    {
        heap.Reset();
    }
    this->heaps.clear();
}
