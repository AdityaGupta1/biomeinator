#include "managed_buffer.h"

#include "buffer_helper.h"

#include <stdexcept>

void ManagedBuffer::init(uint32_t byteSize)
{
    dev_buffer = BufferHelper::createBasicBuffer(
        byteSize, &DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    this->bufferSize = byteSize;

    freeList.push_back({ 0, byteSize });
}

// TODO: keep a persistent rotating pointer into freeList to avoid biasing towards beginning of list for new uploads?
ManagedBufferSection ManagedBuffer::copyFromUploadHeap(ID3D12GraphicsCommandList* cmdList,
                                                       ID3D12Resource* dev_uploadBuffer,
                                                       uint32_t byteSize)
{
    for (auto it = freeList.begin(); it != freeList.end(); ++it)
    {
        if (it->byteSize >= byteSize)
        {
            uint32_t bufferOffset = it->byteOffset;

            BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                         dev_buffer.Get(),
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);

            cmdList->CopyBufferRegion(dev_buffer.Get(), bufferOffset, dev_uploadBuffer, 0, byteSize);

            BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                         dev_buffer.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (it->byteSize == byteSize)
            {
                freeList.erase(it);
            }
            else
            {
                it->byteOffset += byteSize;
                it->byteSize -= byteSize;
            }

            ManagedBufferSection result = {
                .byteOffset = bufferOffset,
                .byteSize = byteSize,
            };
            return result;
        }
    }

    throw std::runtime_error("ManagedBuffer out of space");
}

void ManagedBuffer::free(ManagedBufferSection section)
{
    auto it = freeList.begin();
    for (; it != freeList.end(); ++it)
    {
        if (section.byteOffset + section.byteSize < it->byteOffset)
        {
            break;
        }
    }

    auto inserted = freeList.insert(it, section);

    // try merging with previous
    if (inserted != freeList.begin())
    {
        auto prev = std::prev(inserted);
        if (prev->byteOffset + prev->byteSize == inserted->byteOffset)
        {
            prev->byteSize += inserted->byteSize;
            freeList.erase(inserted);
            inserted = prev;
        }
    }

    // try merging with next
    auto next = std::next(inserted);
    if (next != freeList.end() && inserted->byteOffset + inserted->byteSize == next->byteOffset)
    {
        inserted->byteSize += next->byteSize;
        freeList.erase(next);
    }
}

ID3D12Resource* ManagedBuffer::getBuffer()
{
    return dev_buffer.Get();
}
