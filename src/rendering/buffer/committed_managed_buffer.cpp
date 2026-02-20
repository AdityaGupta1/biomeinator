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

#include "committed_managed_buffer.h"

#include "buffer_helper.h"
#include "to_free_list.h"

CommittedManagedBuffer::CommittedManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                                               D3D12_RESOURCE_STATES initialResourceState,
                                               ManagedBufferOptions options)
    : ManagedBuffer(heapProperties, initialResourceState, options)
{}

void CommittedManagedBuffer::initializeStorage(ToFreeList* toFreeList, size_t sizeBytes)
{
    if (this->initialResourceState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        // have to pass state explicitly for D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
        this->dev_buffer = BufferHelper::createBasicBuffer(
            sizeBytes, this->heapProperties, this->initialResourceState, this->options.bufferCreationFlags);
    }
    else
    {
        this->dev_buffer = BufferHelper::createBasicBuffer(
            sizeBytes, this->heapProperties, this->options.bufferCreationFlags);
    }

    this->bufferSizeBytes = sizeBytes;

    if (this->options.hasSrvDescriptor)
    {
        this->allocSrvDescriptor(toFreeList);
    }

    this->setBufferName();
}

void CommittedManagedBuffer::ensureCapacity(ID3D12GraphicsCommandList* cmdList,
                                             ToFreeList& toFreeList,
                                             size_t minCapacityBytes,
                                             bool useBackFreeSection)
{
    size_t newSizeBytes = 1;
    while (newSizeBytes < minCapacityBytes)
    {
        newSizeBytes *= 2;
    }

    void* host_oldBuffer = this->host_buffer;

    ID3D12Resource* dev_oldBuffer = toFreeList.pushResource(this->dev_buffer, this->options.isMapped);
    const size_t oldSizeBytes = this->bufferSizeBytes;

    this->initializeStorage(&toFreeList, newSizeBytes);

    if (this->options.isMapped)
    {
        // old buffer will be unmapped by ToFreeList
        this->map();
        std::memcpy(this->host_buffer, host_oldBuffer, oldSizeBytes);
    }
    else
    {
        BufferHelper::copyBufferRegion(cmdList,
                                       this->dev_buffer.Get(),
                                       this->initialResourceState,
                                       0,
                                       dev_oldBuffer,
                                       this->initialResourceState,
                                       0,
                                       oldSizeBytes);
    }

    this->extendFreelistCapacity(oldSizeBytes, newSizeBytes, useBackFreeSection);
}

void CommittedManagedBuffer::onReset()
{
    this->dev_buffer.Reset();
    this->bufferSizeBytes = 0;
}
