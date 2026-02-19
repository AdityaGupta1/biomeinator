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

#include "committed_managed_buffer.h"

#include "buffer_helper.h"
#include "to_free_list.h"

CommittedManagedBuffer::CommittedManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                                               D3D12_RESOURCE_STATES initialResourceState,
                                               ManagedBufferOptions options)
    : ManagedBuffer(heapProperties, initialResourceState, options)
{}

void CommittedManagedBuffer::initializeStorage(size_t sizeBytes)
{
    // Mirror the old ManagedBuffer::createBuffer() logic.
    if (this->initialResourceState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        // RAYTRACING_ACCELERATION_STRUCTURE requires the initial state to be passed explicitly
        // because BufferHelper::createBasicBuffer(w, heap, flags) auto-selects the state.
        this->dev_buffer = BufferHelper::createBasicBuffer(
            sizeBytes, this->heapProperties, this->initialResourceState,
            this->options.bufferCreationFlags);
    }
    else
    {
        this->dev_buffer = BufferHelper::createBasicBuffer(
            sizeBytes, this->heapProperties, this->options.bufferCreationFlags);
    }

    this->bufferSizeBytes = sizeBytes;
    this->setBufferName();
}

void CommittedManagedBuffer::ensureCapacity(size_t minCapacityBytes,
                                             bool useBackFreeSection,
                                             ID3D12GraphicsCommandList* cmdList,
                                             ToFreeList& toFreeList)
{
    // Power-of-2 growth – identical to the old ManagedBuffer::resize() policy.
    size_t newSizeBytes = 1;
    while (newSizeBytes < minCapacityBytes)
        newSizeBytes *= 2;

    // Save CPU-side pointer before mapping changes.
    void* host_oldBuffer = this->host_buffer;

    // Hand the old GPU resource off to toFreeList for deferred release.
    // For mapped buffers toFreeList will call Unmap; for unmapped ones it just holds
    // the ComPtr alive until the frame is done.
    ID3D12Resource* dev_oldBuffer = toFreeList.pushResource(this->dev_buffer, this->options.isMapped);
    const size_t oldSizeBytes = this->bufferSizeBytes;

    // Allocate the new (larger) committed resource.
    this->initializeStorage(newSizeBytes);

    if (this->options.isMapped)
    {
        // Map the new buffer immediately; the old one will be unmapped by toFreeList.
        this->map();
        std::memcpy(this->host_buffer, host_oldBuffer, oldSizeBytes);
    }
    else
    {
        // GPU-side copy of old contents into the new resource.
        BufferHelper::copyBufferRegion(cmdList,
                                       this->dev_buffer.Get(),
                                       this->initialResourceState,
                                       0,
                                       dev_oldBuffer,
                                       this->initialResourceState,
                                       0,
                                       oldSizeBytes);
    }

    // Extend the freelist to account for the extra capacity.
    this->extendFreelistCapacity(oldSizeBytes, newSizeBytes, useBackFreeSection);

    // Recreate SRV with the new (larger) element count.
    if (this->options.hasSrvDescriptor)
        this->allocSrvDescriptor(&toFreeList);
}
