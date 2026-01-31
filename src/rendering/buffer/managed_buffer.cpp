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
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"

#include "debug.h"

#include <iterator>

ManagedBufferSection::ManagedBufferSection(ManagedBuffer* buffer, size_t offsetBytes, size_t sizeBytes)
    : buffer(buffer), offsetBytes(offsetBytes), sizeBytes(sizeBytes)
{}

ManagedBufferSection::ManagedBufferSection()
    : ManagedBufferSection(nullptr, 0, 0)
{}

ManagedBuffer* ManagedBufferSection::getBuffer() const
{
    return this->buffer;
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBufferSection::getGpuVirtualAddress() const
{
    return this->getBuffer()->getGpuVirtualAddress() + this->offsetBytes;
}

void ManagedBufferSection::free() const
{
    if (this->sizeBytes > 0)
    {
        this->buffer->freeSection(*this);
    }
}

ManagedBuffer::ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                             const D3D12_RESOURCE_STATES initialResourceState,
                             const ManagedBufferOptions options)
    : heapProperties(heapProperties), initialResourceState(initialResourceState), options(options)
{}

void ManagedBuffer::init(size_t sizeBytes)
{
    ASSERT(sizeBytes > 0);

    this->createBuffer(sizeBytes);

    this->freeByOffset.clear();
    this->freeBySize.clear();
    this->insertFreeNode(0, this->bufferSizeBytes);

    if (this->options.isMapped)
    {
        this->map();
    }

    if (this->options.hasSrvDescriptor)
    {
        this->allocSrvDescriptor(nullptr);
    }
}

void ManagedBuffer::createBuffer(size_t sizeBytes)
{
    // this is a goofy fix but it works and I'm lazy
    if (this->initialResourceState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        this->dev_buffer = BufferHelper::createBasicBuffer(
            sizeBytes, this->heapProperties, this->initialResourceState, this->options.bufferCreationFlags);
    }
    else
    {
        this->dev_buffer =
            BufferHelper::createBasicBuffer(sizeBytes, this->heapProperties, this->options.bufferCreationFlags);
    }

    this->bufferSizeBytes = sizeBytes;
    this->setBufferName();
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

    bool isBufferOccupied = true;
    if (this->freeByOffset.size() == 1)
    {
        const auto offsetIter = this->freeByOffset.begin();
        const auto& freeNode = offsetIter->second;
        const size_t offsetBytes = offsetIter->first;
        if (offsetBytes == 0 && freeNode.sizeBytes == this->bufferSizeBytes)
        {
            isBufferOccupied = false;
        }
    }
    ASSERT(!isBufferOccupied);

    this->dev_buffer.Reset();
    this->bufferSizeBytes = 0;
}

void ManagedBuffer::insertFreeNode(size_t offsetBytes, size_t sizeBytes)
{
    const auto [offsetIter, inserted] = freeByOffset.insert({ offsetBytes, FreeNode{ sizeBytes, {} } });
    ASSERT(inserted, "freeByOffset already contains this offset");
    offsetIter->second.sizeIter = freeBySize.insert({ sizeBytes, offsetIter });
}

void ManagedBuffer::eraseFreeNode(OffsetIter offsetIter)
{
    this->freeBySize.erase(offsetIter->second.sizeIter);
    this->freeByOffset.erase(offsetIter);
}

void ManagedBuffer::allocSrvDescriptor(ToFreeList* toFreeList)
{
    ASSERT(this->options.hasSrvDescriptor);
    ASSERT(toFreeList != nullptr || !this->hasValidSrvDescriptor());

    if (toFreeList != nullptr && this->hasValidSrvDescriptor())
    {
        toFreeList->pushDescriptor(this->srvDescriptorIdx);
    }

    ASSERT(this->options.srvElementByteSize > 0);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = BASIC_SRV_DESC;
    srvDesc.Buffer = {
        .NumElements = static_cast<uint32_t>(this->getSizeBytes() / this->options.srvElementByteSize),
        .StructureByteStride = this->options.srvElementByteSize,
    };
    this->srvDescriptorIdx = Renderer::sharedDescHeapAlloc.alloc(&this->srvDescriptorCpuHandle);
    Renderer::device->CreateShaderResourceView(this->dev_buffer.Get(), &srvDesc, this->srvDescriptorCpuHandle);
}

ManagedBufferSection ManagedBuffer::findFreeSection(ID3D12GraphicsCommandList* cmdList,
                                                    ToFreeList* toFreeList,
                                                    size_t sizeBytes)
{
    const auto sizeIter = this->freeBySize.lower_bound(sizeBytes);
    if (sizeIter != this->freeBySize.end())
    {
        const OffsetIter offsetIter = sizeIter->second;
        const size_t resultOffsetBytes = offsetIter->first;
        const size_t blockSizeBytes = offsetIter->second.sizeBytes;

        this->eraseFreeNode(offsetIter);

        if (blockSizeBytes > sizeBytes)
        {
            const size_t remainderOffsetBytes = resultOffsetBytes + sizeBytes;
            const size_t remainderSizeBytes = blockSizeBytes - sizeBytes;
            this->insertFreeNode(remainderOffsetBytes, remainderSizeBytes);
        }

        return { this, resultOffsetBytes, sizeBytes };
    }

    if (!this->options.isResizable)
    {
        return ManagedBufferSection();
    }

    ASSERT(cmdList != nullptr);
    ASSERT(toFreeList != nullptr);

    // true if the backmost section of the toFreeList is empty and we can resize it to fit the new section
    bool useBackFreeSection = false;
    size_t backSizeBytes = 0;
    if (!this->freeByOffset.empty())
    {
        const auto backIter = std::prev(this->freeByOffset.end());
        const size_t backOffsetBytes = backIter->first;
        const size_t backBlockSizeBytes = backIter->second.sizeBytes;
        if (backOffsetBytes + backBlockSizeBytes == this->bufferSizeBytes)
        {
            useBackFreeSection = true;
            backSizeBytes = backBlockSizeBytes;
        }
    }

    const size_t minNewSizeBytes = this->bufferSizeBytes + sizeBytes - backSizeBytes;
    size_t newSizeBytes = 1;
    while (newSizeBytes < minNewSizeBytes)
    {
        newSizeBytes *= 2;
    }

    this->resize(cmdList, *toFreeList, newSizeBytes, useBackFreeSection);

    return findFreeSection(cmdList, toFreeList, sizeBytes);
}

void ManagedBuffer::resize(ID3D12GraphicsCommandList* cmdList,
                           ToFreeList& toFreeList,
                           size_t newSizeBytes,
                           bool useBackFreeSection)
{
    void* host_oldBuffer = host_buffer;

    ID3D12Resource* dev_oldBuffer = toFreeList.pushResource(this->dev_buffer, this->options.isMapped);
    const size_t oldSizeBytes = this->bufferSizeBytes;

    this->createBuffer(newSizeBytes);

    if (this->options.isMapped)
    {
        this->map(); // dev_oldBuffer will be unmapped by toFreeList

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

    const size_t diffSizeBytes = newSizeBytes - oldSizeBytes;

    if (useBackFreeSection)
    {
        const auto backIter = std::prev(this->freeByOffset.end());
        const size_t newBackSizeBytes = backIter->second.sizeBytes + diffSizeBytes;

        this->freeBySize.erase(backIter->second.sizeIter);

        FreeNode& backFreeNode = backIter->second;
        backFreeNode.sizeBytes = newBackSizeBytes;
        backFreeNode.sizeIter = this->freeBySize.insert({ newBackSizeBytes, backIter });
    }
    else
    {
        this->insertFreeNode(oldSizeBytes, diffSizeBytes);
    }

    if (this->options.hasSrvDescriptor)
    {
        this->allocSrvDescriptor(&toFreeList);
    }
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
    ASSERT(section.getBuffer() == this, "Attempted to free ManagedBufferSection from wrong ManagedBuffer");

    size_t mergedOffsetBytes = section.offsetBytes;
    size_t mergedSizeBytes = section.sizeBytes;

    const OffsetIter nextIter = this->freeByOffset.lower_bound(section.offsetBytes);

    // check previous neighbor for merging
    if (nextIter != this->freeByOffset.begin())
    {
        OffsetIter prevIter = std::prev(nextIter);
        const size_t prevOffsetBytes = prevIter->first;
        const size_t prevSizeBytes = prevIter->second.sizeBytes;
        if (prevOffsetBytes + prevSizeBytes == section.offsetBytes)
        {
            mergedOffsetBytes = prevOffsetBytes;
            mergedSizeBytes = prevSizeBytes + section.sizeBytes;
            this->eraseFreeNode(prevIter);
        }
    }

    // check next neighbor for merging
    if (nextIter != this->freeByOffset.end())
    {
        const size_t nextOffsetBytes = nextIter->first;
        const size_t nextSizeBytes = nextIter->second.sizeBytes;
        if (mergedOffsetBytes + mergedSizeBytes == nextOffsetBytes)
        {
            mergedSizeBytes += nextSizeBytes;
            this->eraseFreeNode(nextIter);
        }
    }

    this->insertFreeNode(mergedOffsetBytes, mergedSizeBytes);
}

ID3D12Resource* ManagedBuffer::getBuffer() const
{
    return this->dev_buffer.Get();
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBuffer::getGpuVirtualAddress() const
{
    return this->dev_buffer->GetGPUVirtualAddress();
}

bool ManagedBuffer::hasValidSrvDescriptor() const
{
    return this->srvDescriptorIdx != ~0u;
}

uint32_t ManagedBuffer::getSrvDescriptorIdx() const
{
    ASSERT(this->options.hasSrvDescriptor);
    ASSERT(this->hasValidSrvDescriptor());
    return this->srvDescriptorIdx;
}

size_t ManagedBuffer::getSizeBytes() const
{
    return this->bufferSizeBytes;
}

void ManagedBuffer::setName(const std::wstring& name)
{
    this->name = name;
}

void ManagedBuffer::setBufferName()
{
    const std::wstring nameWithSize = this->name + L" (size = " + std::to_wstring(this->bufferSizeBytes) + L" bytes)";
    this->dev_buffer->SetName(nameWithSize.c_str());
}
