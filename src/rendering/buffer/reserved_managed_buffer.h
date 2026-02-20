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

#pragma once

#include "managed_buffer.h"

#include "debug.h"

#include <vector>

inline constexpr size_t reservedGrowthChunkBytes = 32ull * 1024 * 1024; // 32 MB

static_assert(reservedGrowthChunkBytes % D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT == 0,
              "reservedGrowthChunkBytes must be a multiple of D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT");

class ReservedManagedBuffer final : public ManagedBuffer
{
public:
    ReservedManagedBuffer(size_t maxReservedSizeBytes,
                          D3D12_RESOURCE_STATES initialResourceState,
                          ManagedBufferOptions options);

protected:
    void initializeStorage(ToFreeList* toFreeList, size_t sizeBytes) override;

    void ensureCapacity(size_t minCapacityBytes,
                        bool useBackFreeSection,
                        ID3D12GraphicsCommandList* cmdList,
                        ToFreeList& toFreeList) override;

    void onReset() override;

private:
    const size_t maxReservedSizeBytes; // virtual size

    size_t mappedCapacityBytes{ 0 }; // TODO: do we need this, or can we just use bufferSizeBytes?

    std::vector<ComPtr<ID3D12Heap>> heaps;

    size_t mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes);
};
