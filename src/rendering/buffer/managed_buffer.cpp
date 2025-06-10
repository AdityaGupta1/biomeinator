#include "managed_buffer.h"

#include "buffer_helper.h"

#include <stdexcept>

ManagedBuffer::ManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                             const D3D12_HEAP_FLAGS heapFlags,
                             const D3D12_RESOURCE_STATES initialResourceState)
    : heapProperties(heapProperties), heapFlags(heapFlags), initialResourceState(initialResourceState)
{}

void ManagedBuffer::init(uint32_t sizeBytes)
{
    dev_buffer =
        BufferHelper::createBasicBuffer(sizeBytes, this->heapProperties, this->heapFlags, this->initialResourceState);
    this->bufferSize = sizeBytes;

    freeList.push_back({ 0, sizeBytes });
}

void ManagedBuffer::resize(uint32_t sizeBytes, ToFreeList& toFreeList)
{
    bool isMapped = this->host_buffer != nullptr;

    if (dev_buffer)
    {
        toFreeList.pushResource(this->dev_buffer, isMapped);
    }

    freeList.clear();
    this->init(sizeBytes);

    if (isMapped)
    {
        this->map();
    }
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
                                                       void* host_srcBuffer,
                                                       uint32_t sizeBytes)
{
    const auto& freeSection = findFreeSection(sizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);

    memcpy(dev_buffer.Get() + freeSection.offsetBytes, host_srcBuffer, sizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);

    return freeSection;
}

ManagedBufferSection ManagedBuffer::copyFromDeviceBuffer(ID3D12GraphicsCommandList* cmdList,
                                                       ID3D12Resource* dev_srcBuffer,
                                                       uint32_t sizeBytes)
{
    const auto& freeSection = findFreeSection(sizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_buffer.Get(), this->initialResourceState, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyBufferRegion(dev_buffer.Get(), freeSection.offsetBytes, dev_srcBuffer, 0, sizeBytes);

    BufferHelper::stateTransitionResourceBarrier(
        cmdList, dev_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, this->initialResourceState);

    return freeSection;
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

ID3D12Resource* ManagedBuffer::getBuffer()
{
    return dev_buffer.Get();
}
