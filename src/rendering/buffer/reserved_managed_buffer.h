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

#include "managed_buffer.h"

#include "debug.h"

#include <vector>

// Hardware tile granularity for D3D12 reserved (tiled) buffer resources.
inline constexpr size_t reservedTileSizeBytes = 65536; // 64 KB

// Minimum growth increment for ReservedManagedBuffer physical backing.
// Must be a multiple of reservedTileSizeBytes (enforced by static_assert below).
inline constexpr size_t reservedGrowthChunkBytes = 32ull * 1024 * 1024; // 32 MB

static_assert(reservedGrowthChunkBytes % reservedTileSizeBytes == 0,
              "reservedGrowthChunkBytes must be a multiple of reservedTileSizeBytes (64 KB)");

// ManagedBuffer backed by a single CreateReservedResource (virtual address space) plus
// a collection of ID3D12Heap objects (physical VRAM).
//
// Key properties:
//   - The GPU resource (dev_buffer) is created once and NEVER recreated.  Its GPU virtual
//     address is therefore stable for the entire lifetime of this object.
//   - Physical memory is allocated lazily in chunks of reservedGrowthChunkBytes by
//     creating new heaps and calling UpdateTileMappings on the graphics queue.
//   - Growing never requires copying old data – the reserved resource covers old and new
//     ranges as one contiguous virtual buffer.
//   - CPU mapping is not supported (reserved resources cannot be mapped).
//     Enforced by asserting options.isMapped == false in the constructor.
//   - If hasSrvDescriptor is set, the SRV is created once using maxReservedSizeBytes as
//     NumElements so it covers the full virtual range and never needs recreation.
//
// Constructor argument maxReservedSizeBytes determines the size of the virtual address
// space.  It must be aligned to reservedTileSizeBytes.  Physical VRAM is not consumed
// until init() is called.
class ReservedManagedBuffer final : public ManagedBuffer
{
public:
    ReservedManagedBuffer(size_t maxReservedSizeBytes,
                          D3D12_RESOURCE_STATES initialResourceState,
                          ManagedBufferOptions options);

protected:
    // Create the reserved resource and map the initial physical heap.
    // Grabs the graphics queue from Renderer::getGraphicsQueue() for UpdateTileMappings.
    void initializeStorage(size_t sizeBytes) override;

    // Map additional physical heaps as needed without recreating the resource.
    void ensureCapacity(size_t minCapacityBytes,
                        bool useBackFreeSection,
                        ID3D12GraphicsCommandList* cmdList,
                        ToFreeList& toFreeList) override;

    // Release the reserved resource and all backing heaps.
    void onReset() override;

private:
    // The full virtual address space width passed at construction time.
    // The GPU resource (dev_buffer) is created with this Width.
    const size_t maxReservedSizeBytes;

    // How many bytes of the virtual space are currently backed by physical heaps.
    // The freelist and bufferSizeBytes (base class) are always kept in sync with this.
    size_t mappedCapacityBytes{ 0 };

    // Each entry is one physical heap allocation.  Heaps are appended as the buffer grows.
    std::vector<ComPtr<ID3D12Heap>> heaps;

    // Allocate a new heap of at least minAdditionalBytes (rounded up to the nearest
    // reservedGrowthChunkBytes) and map its tiles into the reserved resource starting at
    // virtualStartTile.  Returns the actual heap size in bytes.
    size_t mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes);
};
