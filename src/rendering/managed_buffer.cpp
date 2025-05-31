#include "managed_buffer.h"

#include "buffer_helper.h"

void ManagedBuffer::init(uint64_t bufferSize, uint64_t uploadBufferSize)
{
    dev_buffer = BufferHelper::createBasicBuffer(
        bufferSize, &DEFAULT_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    this->bufferSize = bufferSize;

    if (uploadBufferSize > 0)
    {
        dev_uploadBuffer = BufferHelper::createBasicBuffer(
            bufferSize, &UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        dev_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&host_uploadBuffer));
    }
    this->uploadBufferSize = uploadBufferSize;

    freeList.push_back({ 0, bufferSize });
}

// TODO: keep a persistent rotating pointer into freeList to avoid biasing towards beginning of list for new uploads?
ManagedBufferSection ManagedBuffer::upload(ID3D12GraphicsCommandList* cmdList, const void* data, uint64_t size)
{
    for (auto it = freeList.begin(); it != freeList.end(); ++it)
    {
        if (it->size >= size)
        {
            uint64_t bufferOffset = it->offset;

            if (uploadBufferOffset + size > uploadBufferSize)
            {
                uploadBufferOffset = 0;
            }

            std::memcpy(static_cast<uint8_t*>(host_uploadBuffer) + uploadBufferOffset, data, size);

            BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                         dev_buffer.Get(),
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);

            cmdList->CopyBufferRegion(dev_buffer.Get(), bufferOffset, dev_uploadBuffer.Get(), uploadBufferOffset, size);

            BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                         dev_buffer.Get(),
                                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (it->size == size)
            {
                freeList.erase(it);
            }
            else
            {
                it->offset += size;
                it->size -= size;
            }

            uploadBufferOffset += size;

            ManagedBufferSection result = {
                .offset = bufferOffset,
                .size = size,
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
        if (section.offset + section.size < it->offset)
        {
            break;
        }
    }

    auto inserted = freeList.insert(it, section);

    // try merging with previous
    if (inserted != freeList.begin())
    {
        auto prev = std::prev(inserted);
        if (prev->offset + prev->size == inserted->offset)
        {
            prev->size += inserted->size;
            freeList.erase(inserted);
            inserted = prev;
        }
    }

    // try merging with next
    auto next = std::next(inserted);
    if (next != freeList.end() && inserted->offset + inserted->size == next->offset)
    {
        inserted->size += next->size;
        freeList.erase(next);
    }
}

ID3D12Resource* ManagedBuffer::getBuffer()
{
    return dev_buffer.Get();
}
