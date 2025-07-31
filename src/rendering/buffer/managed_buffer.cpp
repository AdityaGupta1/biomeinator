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

#include "managed_buffer.h"

#include "buffer_helper.h"
#include "to_free_list.h"

#include <stdexcept>

ManagedBufferSection::ManagedBufferSection(ManagedBuffer* buffer, uint32_t offsetBytes, uint32_t sizeBytes)
    : buffer(buffer), offsetBytes(offsetBytes), sizeBytes(sizeBytes)
{}

ManagedBufferSection::ManagedBufferSection()
    : ManagedBufferSection(nullptr, 0, 0)
{}

ManagedBuffer* ManagedBufferSection::getBuffer() const
{
    return this->buffer;
}

ManagedBuffer::ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                             const D3D12_RESOURCE_STATES initialResourceState,
                             const bool isResizable,
                             const bool isMapped)
    : heapProperties(heapProperties), initialResourceState(initialResourceState), isResizable(isResizable),
      isMapped(isMapped)
{}

void ManagedBuffer::init(uint32_t sizeBytes)
{
#ifdef _DEBUG
    if (sizeBytes == 0)
    {
        throw std::runtime_error("Attempting to initialize ManagedBuffer with 0 size");
    }
#endif

    this->dev_buffer = BufferHelper::createBasicBuffer(sizeBytes, this->heapProperties, this->initialResourceState);
    this->bufferSizeBytes = sizeBytes;

    this->freeAll();

    if (this->isMapped)
    {
        this->map();
    }
}

void ManagedBuffer::freeAll()
{
    this->freeSectionList.clear();
    this->freeSectionList.push_back({ this, 0, this->bufferSizeBytes });
}

void ManagedBuffer::map()
{
    this->dev_buffer->Map(0, nullptr, &this->host_buffer);
}

void ManagedBuffer::unmap()
{
    this->dev_buffer->Unmap(0, nullptr);
}

// TODO: keep a persistent rotating pointer into freeSectionList to avoid biasing towards beginning of list for new
// uploads? (issue #12)
ManagedBufferSection ManagedBuffer::findFreeSection(ID3D12GraphicsCommandList* cmdList,
                                                    ToFreeList& toFreeList,
                                                    uint32_t sizeBytes)
{
    for (auto it = this->freeSectionList.begin(); it != this->freeSectionList.end(); ++it)
    {
        if (it->sizeBytes >= sizeBytes)
        {
            uint32_t resultOffsetBytes = it->offsetBytes;

            if (it->sizeBytes == sizeBytes)
            {
                this->freeSectionList.erase(it);
            }
            else
            {
                it->offsetBytes += sizeBytes;
                it->sizeBytes -= sizeBytes;
            }

            return { this, resultOffsetBytes, sizeBytes };
        }
    }

#ifdef _DEBUG
    if (!this->isResizable)
    {
        throw std::runtime_error("ManagedBuffer out of space");
    }
#endif

    bool useBackFreeSection = false;
    uint32_t backSizeBytes = 0;
    if (!this->freeSectionList.empty())
    {
        const auto& backSection = this->freeSectionList.back();
        if (backSection.offsetBytes + backSection.sizeBytes == this->bufferSizeBytes)
        {
            useBackFreeSection = true;
            backSizeBytes = backSection.sizeBytes;
        }
    }

    const uint32_t minNewSizeBytes = this->bufferSizeBytes + sizeBytes - backSizeBytes;
    uint32_t newSizeBytes = 1;
    while (newSizeBytes < minNewSizeBytes)
    {
        newSizeBytes *= 2;
    }

    this->resize(cmdList, toFreeList, newSizeBytes, useBackFreeSection);

    return findFreeSection(cmdList, toFreeList, sizeBytes);
}

void ManagedBuffer::resize(ID3D12GraphicsCommandList* cmdList,
                           ToFreeList& toFreeList,
                           uint32_t newSizeBytes,
                           bool useBackFreeSection)
{
#ifdef _DEBUG
    if (this->isMapped)
    {
        throw std::runtime_error("Attempting to resize mapped ManagedBuffer");
    }
#endif

    ID3D12Resource* dev_oldBuffer = toFreeList.pushResource(this->dev_buffer, false);
    const uint32_t oldSizeBytes = this->bufferSizeBytes;

    this->dev_buffer = BufferHelper::createBasicBuffer(newSizeBytes, this->heapProperties, this->initialResourceState);
    this->bufferSizeBytes = newSizeBytes;

    BufferHelper::copyBufferRegion(cmdList,
                                   this->dev_buffer.Get(),
                                   this->initialResourceState,
                                   0,
                                   dev_oldBuffer,
                                   this->initialResourceState,
                                   0,
                                   oldSizeBytes);

    const uint32_t diffSizeBytes = newSizeBytes - oldSizeBytes;

    if (useBackFreeSection)
    {
        this->freeSectionList.back().sizeBytes += diffSizeBytes;
    }
    else
    {
        this->freeSectionList.push_back({ this, oldSizeBytes, diffSizeBytes });
    }
}

ManagedBufferSection ManagedBuffer::copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                                       ToFreeList& toFreeList,
                                                       const void* host_srcBuffer,
                                                       uint32_t sizeBytes)
{
#ifdef _DEBUG
    if (!this->isMapped)
    {
        throw std::runtime_error("Attempting to copy from host buffer to unmapped ManagedBuffer");
    }
#endif

    const auto& freeSection = this->findFreeSection(cmdList, toFreeList, sizeBytes);

    memcpy((uint8_t*)host_buffer + freeSection.offsetBytes, host_srcBuffer, sizeBytes);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                                         ToFreeList& toFreeList,
                                                         ID3D12Resource* dev_srcBuffer,
                                                         uint32_t srcSizeBytes,
                                                         uint32_t srcOffsetBytes)
{
    const auto& freeSection = this->findFreeSection(cmdList, toFreeList, srcSizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(
        this->dev_buffer.Get(), freeSection.offsetBytes, dev_srcBuffer, srcOffsetBytes, srcSizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                                          ToFreeList& toFreeList,
                                                          const ManagedBuffer& srcBuffer,
                                                          ManagedBufferSection srcBufferSection)
{
    return this->copyFromDeviceBuffer(cmdList,
                                      toFreeList,
                                      srcBuffer.getBuffer(),
                                      srcBufferSection.sizeBytes,
                                      srcBufferSection.offsetBytes);
}

void ManagedBuffer::freeSection(ManagedBufferSection section)
{
#ifdef _DEBUG
    if (section.getBuffer() != this)
    {
        throw std::runtime_error("Attempting to free ManagedBufferSection from wrong ManagedBuffer");
    }
#endif

    auto it = this->freeSectionList.begin();
    for (; it != this->freeSectionList.end(); ++it)
    {
        if (section.offsetBytes + section.sizeBytes < it->offsetBytes)
        {
            break;
        }
    }

    auto inserted = this->freeSectionList.insert(it, section);

    // try merging with previous
    if (inserted != this->freeSectionList.begin())
    {
        auto prev = std::prev(inserted);
        if (prev->offsetBytes + prev->sizeBytes == inserted->offsetBytes)
        {
            prev->sizeBytes += inserted->sizeBytes;
            this->freeSectionList.erase(inserted);
            inserted = prev;
        }
    }

    // try merging with next
    auto next = std::next(inserted);
    if (next != this->freeSectionList.end() && inserted->offsetBytes + inserted->sizeBytes == next->offsetBytes)
    {
        inserted->sizeBytes += next->sizeBytes;
        this->freeSectionList.erase(next);
    }
}

ID3D12Resource* ManagedBuffer::getBuffer() const
{
    return this->dev_buffer.Get();
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBuffer::getBufferGpuAddress() const
{
    return this->dev_buffer->GetGPUVirtualAddress();
}

uint32_t ManagedBuffer::getSizeBytes() const
{
    return this->bufferSizeBytes;
}
