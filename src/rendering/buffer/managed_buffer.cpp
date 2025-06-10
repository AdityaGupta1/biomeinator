#include "managed_buffer.h"

#include "buffer_helper.h"

#include <stdexcept>

ManagedBuffer::ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                             const D3D12_HEAP_FLAGS heapFlags,
                             const D3D12_RESOURCE_STATES initialResourceState,
                             const bool isMapped)
    : heapProperties(heapProperties), heapFlags(heapFlags), initialResourceState(initialResourceState),
      isMapped(isMapped)
{}

void ManagedBuffer::init(uint32_t sizeBytes)
{
    dev_buffer =
        BufferHelper::createBasicBuffer(sizeBytes, this->heapProperties, this->heapFlags, this->initialResourceState);
    this->bufferSizeBytes = sizeBytes;

    freeList.push_back({ 0, sizeBytes });

    if (this->isMapped)
    {
        this->map();
    }
}

void ManagedBuffer::resize(uint32_t sizeBytes, ToFreeList& toFreeList, bool canResizeSmaller)
{
    if (!canResizeSmaller && sizeBytes < this->bufferSizeBytes)
    {
        return;
    }

    if (dev_buffer)
    {
        toFreeList.pushResource(this->dev_buffer, this->isMapped);
    }

    freeList.clear();
    this->init(sizeBytes);
}

void ManagedBuffer::map()
{
    dev_buffer->Map(0, nullptr, &host_buffer);
}

void ManagedBuffer::unmap()
{
    dev_buffer->Unmap(0, nullptr);
}

// TODO: keep a persistent rotating pointer into freeList to avoid biasing towards beginning of list for new uploads?
ManagedBufferSection ManagedBuffer::findFreeSection(uint32_t sizeBytes)
{
    for (auto it = freeList.begin(); it != freeList.end(); ++it)
    {
        if (it->sizeBytes >= sizeBytes)
        {
            uint32_t resultOffsetBytes = it->offsetBytes;

            if (it->sizeBytes == sizeBytes)
            {
                freeList.erase(it);
            }
            else
            {
                it->offsetBytes += sizeBytes;
                it->sizeBytes -= sizeBytes;
            }

            ManagedBufferSection result = {
                .offsetBytes = resultOffsetBytes,
                .sizeBytes = sizeBytes,
            };
            return result;
        }
    }

    throw std::runtime_error("ManagedBuffer out of space");
}

ManagedBufferSection ManagedBuffer::copyFromHostBuffer(ID3D12GraphicsCommandList* cmdList,
                                                       const void* host_srcBuffer,
                                                       uint32_t sizeBytes)
{
    if (!isMapped)
    {
        throw std::runtime_error("Attempting to copy from host buffer to unmapped ManagedBuffer");
    }

    const auto& freeSection = findFreeSection(sizeBytes);

    memcpy((uint8_t*)host_buffer + freeSection.offsetBytes, host_srcBuffer, sizeBytes);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                                         ID3D12Resource* dev_srcBuffer,
                                                         uint32_t srcSizeBytes,
                                                         uint32_t srcOffsetBytes)
{
    const auto& freeSection = findFreeSection(srcSizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(
        this->dev_buffer.Get(), freeSection.offsetBytes, dev_srcBuffer, srcOffsetBytes, srcSizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, this->dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromManagedBuffer(ID3D12GraphicsCommandList* cmdList,
                                                          const ManagedBuffer& dev_srcBuffer,
                                                          ManagedBufferSection srcBufferSection)
{
    return this->copyFromDeviceBuffer(
        cmdList, dev_srcBuffer.getBuffer(), srcBufferSection.sizeBytes, srcBufferSection.offsetBytes);
}

void ManagedBuffer::freeSection(ManagedBufferSection section)
{
    auto it = freeList.begin();
    for (; it != freeList.end(); ++it)
    {
        if (section.offsetBytes + section.sizeBytes < it->offsetBytes)
        {
            break;
        }
    }

    auto inserted = freeList.insert(it, section);

    // try merging with previous
    if (inserted != freeList.begin())
    {
        auto prev = std::prev(inserted);
        if (prev->offsetBytes + prev->sizeBytes == inserted->offsetBytes)
        {
            prev->sizeBytes += inserted->sizeBytes;
            freeList.erase(inserted);
            inserted = prev;
        }
    }

    // try merging with next
    auto next = std::next(inserted);
    if (next != freeList.end() && inserted->offsetBytes + inserted->sizeBytes == next->offsetBytes)
    {
        inserted->sizeBytes += next->sizeBytes;
        freeList.erase(next);
    }
}

ID3D12Resource* ManagedBuffer::getBuffer() const
{
    return dev_buffer.Get();
}

D3D12_GPU_VIRTUAL_ADDRESS ManagedBuffer::getBufferGpuAddress() const
{
    return dev_buffer->GetGPUVirtualAddress();
}

uint32_t ManagedBuffer::getSizeBytes() const
{
    return bufferSizeBytes;
}
