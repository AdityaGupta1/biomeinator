// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "managed_buffer.h"

#include "buffer_helper.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "to_free_list.h"

#include "debug.h"

ManagedBufferSection::ManagedBufferSection(ManagedBuffer* buffer, size_t offsetBytes, size_t sizeBytes)
    : buffer(buffer), offsetBytes(offsetBytes), sizeBytes(sizeBytes)
{
}

ManagedBufferSection::ManagedBufferSection() : ManagedBufferSection(nullptr, 0, 0)
{
}

ManagedBuffer* ManagedBufferSection::getBuffer() const
{
    return this->buffer;
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBufferSection::getGpuVirtualAddress() const
{
    return this->getBuffer()->getGpuVirtualAddress() + this->offsetBytes;
}

void ManagedBufferSection::free()
{
    if (this->sizeBytes > 0)
    {
        this->buffer->freeSection(*this);
        *this = {};
    }
}

ManagedBuffer::ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                             const D3D12_RESOURCE_STATES initialResourceState,
                             const ManagedBufferOptions options)
    : heapProperties(heapProperties), initialResourceState(initialResourceState), options(options),
      freeRanges(options.alignmentBytes)
{
}

void ManagedBuffer::init(size_t sizeBytes)
{
    ASSERT(sizeBytes > 0);

    this->initializeStorage(nullptr /*toFreeList*/, sizeBytes);

    this->freeRanges.reset(this->bufferSizeBytes);

    if (this->options.isMapped)
    {
        this->map();
    }
}

void ManagedBuffer::map()
{
    this->dev_buffer->Map(0, nullptr, &this->host_buffer);
}

void ManagedBuffer::unmap()
{
    this->dev_buffer->Unmap(0, nullptr);
}

void ManagedBuffer::reset()
{
    if (this->options.isMapped)
    {
        this->unmap();
    }

    ASSERT(this->freeRanges.isCompletelyFree(), "ManagedBuffer reset while sections are still allocated");

    this->onReset();
    this->bufferSizeBytes = 0;
    this->freeRanges.reset(0);
}

void ManagedBuffer::freeSection(ManagedBufferSection section)
{
    const bool ownsSection = section.getBuffer() == this;
    ASSERT(ownsSection, "Attempted to free ManagedBufferSection from wrong ManagedBuffer");
    if (!ownsSection)
    {
        return;
    }
    const bool didRelease = this->freeRanges.release({ section.offsetBytes, section.sizeBytes });
    ASSERT(didRelease, "Attempted to free an invalid or already-free ManagedBufferSection");
}

void ManagedBuffer::setBufferName()
{
    const std::wstring nameWithSize = this->name + L" (size = " + std::to_wstring(this->bufferSizeBytes) + L" bytes)";
    this->dev_buffer->SetName(nameWithSize.c_str());
}

void ManagedBuffer::onReset()
{
    this->dev_buffer.Reset();
}

ManagedBufferSection ManagedBuffer::findFreeSection(ID3D12GraphicsCommandList* cmdList,
                                                    ToFreeList* toFreeList,
                                                    size_t sizeBytes)
{
    if (const std::optional<FreeRange> range = this->freeRanges.allocate(sizeBytes))
    {
        return { this, range->offsetBytes, range->sizeBytes };
    }

    if (!this->options.isResizable)
    {
        return ManagedBufferSection();
    }

    const size_t allocationSizeBytes = this->freeRanges.getAllocationSize(sizeBytes);
    if (allocationSizeBytes == 0)
    {
        return ManagedBufferSection();
    }

    ASSERT(cmdList != nullptr);
    ASSERT(toFreeList != nullptr);

    const size_t minNewSizeBytes = this->bufferSizeBytes + allocationSizeBytes - this->freeRanges.getFreeTailBytes();
    this->ensureCapacity(cmdList, *toFreeList, minNewSizeBytes);

    const bool didGrow = this->freeRanges.grow(this->bufferSizeBytes);
    ASSERT(didGrow, "ManagedBuffer storage did not grow to the requested capacity");
    if (!didGrow)
    {
        return ManagedBufferSection();
    }

    return findFreeSection(cmdList, toFreeList, sizeBytes);
}

ManagedBufferSection ManagedBuffer::copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                                       ToFreeList& toFreeList,
                                                       const void* host_srcBuffer,
                                                       size_t sizeBytes)
{
    ASSERT(this->options.isMapped, "Cannot copy from host buffer to unmapped ManagedBuffer");

    const ManagedBufferSection& freeSection = this->findFreeSection(cmdList, &toFreeList, sizeBytes);

    memcpy((uint8_t*)this->host_buffer + freeSection.offsetBytes, host_srcBuffer, sizeBytes);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                                         ToFreeList& toFreeList,
                                                         ID3D12Resource* dev_srcBuffer,
                                                         size_t srcSizeBytes,
                                                         size_t srcOffsetBytes)
{
    const ManagedBufferSection& freeSection = this->findFreeSection(cmdList, &toFreeList, srcSizeBytes);

    if (!this->batchCopyActive)
    {
        BufferHelper::stateTransitionResourceBarrier(
            cmdList, this->dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    cmdList->CopyBufferRegion(
        this->dev_buffer.Get(), freeSection.offsetBytes, dev_srcBuffer, srcOffsetBytes, srcSizeBytes);

    if (!this->batchCopyActive)
    {
        BufferHelper::stateTransitionResourceBarrier(
            cmdList, this->dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);
    }

    return freeSection;
}

void ManagedBuffer::beginBatchCopy(ID3D12GraphicsCommandList* cmdList)
{
    ASSERT(!this->options.isMapped, "beginBatchCopy is not valid for mapped buffers");
    ASSERT(!this->batchCopyActive);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);
    this->batchCopyActive = true;
}

void ManagedBuffer::endBatchCopy(ID3D12GraphicsCommandList* cmdList)
{
    ASSERT(this->batchCopyActive);
    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);
    this->batchCopyActive = false;
}

ManagedBufferSection ManagedBuffer::copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                                          ToFreeList& toFreeList,
                                                          const ManagedBuffer& srcBuffer,
                                                          ManagedBufferSection srcBufferSection)
{
    return this->copyFromDeviceBuffer(
        cmdList, toFreeList, srcBuffer.getBuffer(), srcBufferSection.sizeBytes, srcBufferSection.offsetBytes);
}

ID3D12Resource* ManagedBuffer::getBuffer() const
{
    return this->dev_buffer.Get();
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBuffer::getGpuVirtualAddress() const
{
    return this->dev_buffer->GetGPUVirtualAddress();
}

size_t ManagedBuffer::getSizeBytes() const
{
    return this->bufferSizeBytes;
}

size_t ManagedBuffer::getFreeBytes() const
{
    return this->freeRanges.getFreeBytes();
}

GpuMemoryEntry ManagedBuffer::reportGpuMemory() const
{
    return {
        .name = Util::to_string(this->name.c_str()),
        .allocatedBytes = this->bufferSizeBytes,
        .usedBytes = this->bufferSizeBytes - this->getFreeBytes(),
    };
}

void ManagedBuffer::setName(const std::wstring& name)
{
    this->name = name;
}
