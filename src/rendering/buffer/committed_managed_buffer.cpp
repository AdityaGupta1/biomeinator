// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
    // Specifically not passing in initialResourceState here as that's used only for internal tracking. For example,
    // even if the initialResourceState is D3D12_RESOURCE_STATE_UNORDERED_ACCESS, the resource should be created in
    // D3D12_RESOURCE_STATE_COMMON.
    this->dev_buffer =
        BufferHelper::createBasicBuffer(sizeBytes, this->heapProperties, this->options.bufferCreationFlags);

    this->bufferSizeBytes = sizeBytes;

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

    ID3D12Resource* dev_oldBuffer = toFreeList.pushResource(this->dev_buffer);
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
