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

#pragma once

#include "rendering/renderer.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/descriptor_heap_allocator.h"
#include "rendering/buffer/to_free_list.h"

#include "debug.h"

struct MappedArrayOptions
{
    bool hasSrvDescriptor{ false };
};

template<typename T> class MappedArray
{
private:
    const MappedArrayOptions options;

    uint32_t size{ 0 };
    T* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> upload_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };

    uint32_t dirtyBeginIdx{ 0 };
    uint32_t dirtyEndIdx{ 0 };

    uint32_t srvDescriptorIdx{ ~0u };
    D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptorCpuHandle{};

    void setNotDirty()
    {
        this->dirtyBeginIdx = this->size;
        this->dirtyEndIdx = 0;
    }

    void allocSrvDescriptor(ToFreeList* toFreeList)
    {
        ASSERT(this->options.hasSrvDescriptor);
        ASSERT(toFreeList != nullptr || !this->hasValidSrvDescriptor());

        if (toFreeList != nullptr && this->hasValidSrvDescriptor())
        {
            toFreeList->pushDescriptor(this->srvDescriptorIdx);
        }

        const D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = DXGI_FORMAT_UNKNOWN,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .NumElements = this->size,
                .StructureByteStride = sizeof(T),
            },
        };
        this->srvDescriptorIdx = Renderer::sharedDescHeapAlloc.alloc(&this->srvDescriptorCpuHandle);
        Renderer::device->CreateShaderResourceView(this->dev_buffer.Get(), &srvDesc, this->srvDescriptorCpuHandle);
    }

    void init(uint32_t size, ToFreeList* toFreeList)
    {
        this->size = size;
        const uint32_t sizeBytes = sizeof(T) * size;

        this->upload_buffer = BufferHelper::createBasicBuffer(sizeBytes, &UPLOAD_HEAP);
        this->upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&host_buffer));

        this->dev_buffer = BufferHelper::createBasicBuffer(sizeBytes, &DEFAULT_HEAP);

        this->setNotDirty();

        if (this->options.hasSrvDescriptor)
        {
            this->allocSrvDescriptor(toFreeList);
        }
    }

public:
    MappedArray(MappedArrayOptions options)
        : options(options)
    {}

    void init(uint32_t size)
    {
        this->init(size, nullptr);
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

        this->init(newSize, &toFreeList);

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

    bool hasValidSrvDescriptor() const
    {
        return this->srvDescriptorIdx != ~0u;
    }

    uint32_t getSrvDescriptorIdx() const
    {
        ASSERT(this->options.hasSrvDescriptor);
        ASSERT(this->hasValidSrvDescriptor());
        return this->srvDescriptorIdx;
    }
};
