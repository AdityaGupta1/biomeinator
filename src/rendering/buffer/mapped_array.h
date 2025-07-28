#pragma once

#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"

template<class T> class MappedArray
{
private:
    uint32_t size;
    T* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> upload_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };

    uint32_t dirtyBeginIdx{ 0 };
    uint32_t dirtyEndIdx{ 0 };

    void setNotDirty()
    {
        this->dirtyBeginIdx = this->size;
        this->dirtyEndIdx = 0;
    }

public:
    void init(uint32_t size)
    {
        this->size = size;
        const uint32_t sizeBytes = sizeof(T) * size;

        upload_buffer = BufferHelper::createBasicBuffer(sizeBytes, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
        upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&host_buffer));

        dev_buffer =
            BufferHelper::createBasicBuffer(sizeBytes, &DEFAULT_HEAP, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        this->setNotDirty();
    }

    T& operator[](uint32_t idx)
    {
#ifdef _DEBUG
        if (idx >= this->size)
        {
            throw std::exception("MappedArray access out of bounds");
        }
#endif

        this->dirtyBeginIdx = std::min(this->dirtyBeginIdx, idx);
        this->dirtyEndIdx = std::max(this->dirtyEndIdx, idx + 1);
        return host_buffer[idx];
    }

    void copyFromUploadBufferIfDirty(ID3D12GraphicsCommandList* cmdList)
    {
        if (!this->getIsDirty())
        {
            return;
        }

        const uint32_t startBytes = sizeof(T) * this->dirtyBeginIdx;
        const uint32_t sizeBytes = sizeof(T) * (this->dirtyEndIdx - this->dirtyBeginIdx);

        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyBufferRegion(this->dev_buffer.Get(), startBytes, this->upload_buffer.Get(), startBytes, sizeBytes);
        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        this->setNotDirty();
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

        this->dirtyBeginIdx = 0;
        this->dirtyEndIdx = newSize;
    }

    inline uint32_t getSize() const
    {
        return this->size;
    }

    inline bool getIsDirty() const
    {
        return this->dirtyBeginIdx <= this->dirtyEndIdx;
    }

    inline ID3D12Resource* getUploadBuffer() const
    {
        return this->upload_buffer.Get();
    }

    inline ID3D12Resource* getBuffer() const
    {
        return this->dev_buffer.Get();
    }

    inline D3D12_GPU_VIRTUAL_ADDRESS getBufferGpuAddress() const
    {
        return this->dev_buffer->GetGPUVirtualAddress();
    }
};
