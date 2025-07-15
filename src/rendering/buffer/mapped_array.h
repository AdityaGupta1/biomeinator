#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"

template<class T> class MappedArray
{
private:
    uint32_t size;
    T* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> upload_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };

    bool isDirty{ false };

public:
    void init(uint32_t size)
    {
        this->size = size;
        const uint32_t sizeBytes = sizeof(T) * size;

        upload_buffer = BufferHelper::createBasicBuffer(sizeBytes, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&host_buffer));

        dev_buffer =
            BufferHelper::createBasicBuffer(sizeBytes, &DEFAULT_HEAP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    T& operator[](uint32_t idx)
    {
        this->isDirty = true; // kind of hacky but if you're accessing this then you probably want to do a copy too
        return host_buffer[idx];
    }

    void copyFromUploadBuffer(ID3D12GraphicsCommandList* cmdList)
    {
        const uint32_t sizeBytes = sizeof(T) * size;

        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyBufferRegion(this->dev_buffer.Get(), 0, this->upload_buffer.Get(), 0, sizeBytes);
        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        this->isDirty = false;
    }

    void resize(ToFreeList& toFreeList, uint32_t newSize)
    {
        uint32_t oldSize = this->size;
        T* host_oldBuffer = this->host_buffer;

        toFreeList.pushResource(this->upload_buffer, true);
        toFreeList.pushResource(this->dev_buffer, false);

        this->init(newSize);

        const uint32_t copyCount = std::min(oldSize, newSize);
        memcpy(this->host_buffer, host_oldBuffer, sizeof(T) * copyCount);

        this->isDirty = true;
    }

    inline uint32_t getSize()
    {
        return this->size;
    }

    inline bool getIsDirty()
    {
        return this->isDirty;
    }

    inline ID3D12Resource* getUploadBuffer()
    {
        return this->upload_buffer.Get();
    }

    inline ID3D12Resource* getBuffer()
    {
        return this->dev_buffer.Get();
    }
};