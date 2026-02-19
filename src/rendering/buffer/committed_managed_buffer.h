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

// ManagedBuffer backed by a single CreateCommittedResource allocation.
//
// Resize behaviour: allocate a new committed resource at the new (larger) size,
// copy old contents via GPU CopyBufferRegion (or memcpy for CPU-mapped buffers),
// then defer freeing the old resource through ToFreeList.
//
// This preserves the original ManagedBuffer resize semantics exactly.
class CommittedManagedBuffer final : public ManagedBuffer
{
public:
    // Constructor signature is identical to the old ManagedBuffer constructor so that
    // existing construction callsites only need a type-name change.
    CommittedManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                           D3D12_RESOURCE_STATES initialResourceState,
                           ManagedBufferOptions options);

protected:
    // Create the committed resource and set dev_buffer / bufferSizeBytes.
    void initializeStorage(size_t sizeBytes) override;

    // Grow to at least minCapacityBytes using power-of-2 doubling.
    // Recreates the committed resource, copies old contents, updates the freelist.
    void ensureCapacity(size_t minCapacityBytes,
                        bool useBackFreeSection,
                        ID3D12GraphicsCommandList* cmdList,
                        ToFreeList& toFreeList) override;

    // onReset() is not overridden – the base default (dev_buffer.Reset()) is correct.
};
