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

#include <algorithm> // std::max

ReservedManagedBuffer::ReservedManagedBuffer(size_t maxReservedSizeBytes,
                                             D3D12_RESOURCE_STATES initialResourceState,
                                             ManagedBufferOptions options)
    : ManagedBuffer(nullptr /*heapProperties*/,
        initialResourceState,
        options)
    , maxReservedSizeBytes(maxReservedSizeBytes)
{
    ASSERT(!options.isMapped, "ReservedManagedBuffer cannot be CPU-mapped");
    ASSERT(maxReservedSizeBytes > 0);
    ASSERT(maxReservedSizeBytes % kReservedTileSizeBytes == 0,
           "maxReservedSizeBytes must be aligned to kReservedTileSizeBytes (64 KB)");
}

// ---------------------------------------------------------------------------
// initializeStorage
// ---------------------------------------------------------------------------

void ReservedManagedBuffer::initializeStorage(size_t sizeBytes)
{
    ASSERT(Renderer::getGraphicsQueue() != nullptr,
           "Renderer graphics queue must be initialised before ReservedManagedBuffer::init");

    // -----------------------------------------------------------------------
    // Create the virtual reserved resource.
    //
    // CreateReservedResource allocates a contiguous GPU virtual address range of
    // maxReservedSizeBytes bytes but commits NO physical VRAM.  Physical memory is
    // supplied later via ID3D12Heap objects and UpdateTileMappings.
    //
    // BASIC_BUFFER_DESC already has Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    // which is required for reserved buffer resources.
    // -----------------------------------------------------------------------
    D3D12_RESOURCE_DESC resDesc = BASIC_BUFFER_DESC;
    resDesc.Width = static_cast<UINT64>(maxReservedSizeBytes);
    resDesc.Flags = this->options.bufferCreationFlags.resourceFlags;

    CHECK_HRESULT(Renderer::getDevice()->CreateReservedResource(
        &resDesc,
        this->initialResourceState,
        nullptr, // no optimised clear value for buffers
        IID_PPV_ARGS(&this->dev_buffer)));

    this->setBufferName();

    // -----------------------------------------------------------------------
    // Immediately map physical tiles to back at least sizeBytes.
    // -----------------------------------------------------------------------
    const size_t heapSize = mapNewHeap(/*virtualStartTile=*/0, sizeBytes);
    this->mappedCapacityBytes = heapSize;

    // bufferSizeBytes (base class) tracks the *mapped* (physically accessible) range.
    // The freelist will be initialised over [0, bufferSizeBytes) by ManagedBuffer::init().
    this->bufferSizeBytes = this->mappedCapacityBytes;

    // -----------------------------------------------------------------------
    // SRV (if requested).
    //
    // Use maxReservedSizeBytes for NumElements so the descriptor covers the full
    // virtual range and NEVER needs to be recreated when physical backing grows.
    // -----------------------------------------------------------------------
    if (this->options.hasSrvDescriptor)
    {
        this->allocSrvDescriptor(nullptr, maxReservedSizeBytes);
    }
}

// ---------------------------------------------------------------------------
// mapNewHeap – private helper
// ---------------------------------------------------------------------------

size_t ReservedManagedBuffer::mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes)
{
    // Round up to the nearest kReservedGrowthChunkBytes.
    // Because kReservedGrowthChunkBytes is a multiple of kReservedTileSizeBytes (enforced by
    // static_assert in the header), this rounding is automatically tile-aligned too.
    const size_t chunkSize = kReservedGrowthChunkBytes;
    size_t heapSize = std::max(chunkSize, minAdditionalBytes);
    heapSize = (heapSize + chunkSize - 1) & ~(chunkSize - 1);

    ASSERT(virtualStartTile * kReservedTileSizeBytes + heapSize <= maxReservedSizeBytes,
           "ReservedManagedBuffer: virtual address space exhausted - increase maxReservedSizeBytes");

    // -----------------------------------------------------------------------
    // Allocate a physical heap.
    //
    // The heap holds real VRAM.  D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS is required
    // when mixing buffer and non-buffer resources on D3D12_HEAP_TYPE_DEFAULT.
    // -----------------------------------------------------------------------
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = static_cast<UINT64>(heapSize);
    heapDesc.Properties = { D3D12_HEAP_TYPE_DEFAULT };
    heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    ComPtr<ID3D12Heap> heap;
    CHECK_HRESULT(Renderer::getDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)));

    // -----------------------------------------------------------------------
    // Map virtual tiles → heap tiles via UpdateTileMappings.
    //
    // This tells the GPU HW:
    //   reserved resource virtual tiles [virtualStartTile, virtualStartTile + tileCount)
    //   → heap physical tiles [0, tileCount)
    //
    // UpdateTileMappings executes on the command queue immediately (no command list
    // involved).  Because mapping and subsequent draw calls are submitted to the same
    // queue, queue serialisation guarantees the mapping is visible before any draw.
    // No extra fencing is needed.
    // -----------------------------------------------------------------------
    const UINT tileCount  = static_cast<UINT>(heapSize / kReservedTileSizeBytes);
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

// ---------------------------------------------------------------------------
// ensureCapacity
// ---------------------------------------------------------------------------

void ReservedManagedBuffer::ensureCapacity(size_t minCapacityBytes,
                                            bool useBackFreeSection,
                                            ID3D12GraphicsCommandList* /*cmdList*/,
                                            ToFreeList& /*toFreeList*/)
{
    if (this->mappedCapacityBytes >= minCapacityBytes)
        return;

    const size_t additionalNeeded = minCapacityBytes - this->mappedCapacityBytes;
    const size_t virtualStartTile = this->mappedCapacityBytes / kReservedTileSizeBytes;

    const size_t oldMapped = this->mappedCapacityBytes;

    // Allocate a new physical heap and map it into the next virtual tile range.
    // The reserved resource (dev_buffer) and its GPU virtual address are UNCHANGED –
    // no data copy or resource recreation is needed.
    const size_t heapSize = mapNewHeap(virtualStartTile, additionalNeeded);
    this->mappedCapacityBytes += heapSize;

    // Keep base-class bufferSizeBytes in sync with the newly mapped capacity.
    this->bufferSizeBytes = this->mappedCapacityBytes;

    // Extend the freelist to cover the newly mapped region.
    this->extendFreelistCapacity(oldMapped, this->mappedCapacityBytes, useBackFreeSection);

    // SRV is NOT recreated – it was created with maxReservedSizeBytes NumElements so it
    // already covers the entire virtual address range.
}

// ---------------------------------------------------------------------------
// onReset
// ---------------------------------------------------------------------------

void ReservedManagedBuffer::onReset()
{
    // Release the virtual reserved resource first.
    this->dev_buffer.Reset();

    // Release all physical backing heaps.
    for (auto& heap : this->heaps)
    {
        heap.Reset();
    }
    this->heaps.clear();

    this->mappedCapacityBytes = 0;
}
