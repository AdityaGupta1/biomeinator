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

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/to_free_list.h"

#include "debug.h"

struct MappedArrayOptions
{
    bool uploadOnly{ false }; // only allocate upload_buffer; dev_buffer is unused
};

template<typename T> class MappedArray
{
private:
    std::wstring name{ L"MappedArray" };

    MappedArrayOptions options{};

    uint32_t size{ 0 };
    T* host_buffer{ nullptr };
    ComPtr<ID3D12Resource> upload_buffer{ nullptr };
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };

    struct DirtyRange
    {
        uint32_t begin;
        uint32_t end; // exclusive
    };
    std::vector<DirtyRange> dirtyRanges;

    void setNotDirty()
    {
        this->dirtyRanges.clear();
    }

    void insertDirtyRange(uint32_t newRangeBegin, uint32_t newRangeEnd)
    {
        if (this->dirtyRanges.empty())
        {
            this->dirtyRanges.emplace_back(newRangeBegin, newRangeEnd);
            return;
        }

        // skip binary search if new range is at or after all existing ranges
        DirtyRange& last = this->dirtyRanges.back();
        if (newRangeBegin >= last.end)
        {
            if (newRangeBegin == last.end)
            {
                last.end = newRangeEnd; // adjacent to last range, extend it
            }
            else
            {
                this->dirtyRanges.push_back({ newRangeBegin, newRangeEnd }); // after last range, append new one
            }
            return;
        }

        // find first existing range whose .end >= newRangeBegin (might overlap or be adjacent)
        auto it = std::lower_bound(this->dirtyRanges.begin(),
                                   this->dirtyRanges.end(),
                                   newRangeBegin,
                                   [](const DirtyRange& r, uint32_t val) { return r.end < val; });

        // no overlapping or adjacent range found, so insert a new one
        if (it == this->dirtyRanges.end() || it->begin > newRangeEnd)
        {
            this->dirtyRanges.insert(it, { newRangeBegin, newRangeEnd });
            return;
        }

        // merge into found range
        it->begin = std::min(it->begin, newRangeBegin);
        it->end = std::max(it->end, newRangeEnd);

        // absorb any subsequent overlapping ranges
        auto next = std::next(it);
        while (next != this->dirtyRanges.end() && next->begin <= it->end)
        {
            it->end = std::max(it->end, next->end);
            next = this->dirtyRanges.erase(next);
        }
    }

    void init(uint32_t size, ToFreeList* toFreeList)
    {
        this->size = size;
        const uint32_t sizeBytes = sizeof(T) * size;

        const std::wstring sizeStr = L"(size = " + std::to_wstring(sizeBytes) + L" bytes) ";

        this->upload_buffer = BufferHelper::createBasicBuffer(sizeBytes, &UPLOAD_HEAP);
        this->upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&host_buffer));
        const std::wstring uploadBufferNameWithSize = this->name + L" upload_buffer" + sizeStr;
        this->upload_buffer->SetName(uploadBufferNameWithSize.c_str());

        if (!this->options.uploadOnly)
        {
            this->dev_buffer = BufferHelper::createBasicBuffer(sizeBytes, &DEFAULT_HEAP);
            const std::wstring devBufferNameWithSize = this->name + L" dev_buffer" + sizeStr;
            this->dev_buffer->SetName(devBufferNameWithSize.c_str());
        }

        this->setNotDirty();
    }

public:
    inline void init(uint32_t size)
    {
        this->init(size, nullptr);
    }

    inline void init(MappedArrayOptions options, uint32_t size)
    {
        this->options = options;
        this->init(size, nullptr);
    }

    T& operator[](uint32_t idx)
    {
        ASSERT(idx < this->size);
        return host_buffer[idx];
    }

    inline void markDirty(uint32_t idx)
    {
        this->insertDirtyRange(idx, idx + 1);
    }

    inline void markDirtyRange(uint32_t begin, uint32_t end)
    {
        this->insertDirtyRange(begin, end);
    }

    bool copyFromUploadBufferIfDirty(ID3D12GraphicsCommandList* cmdList)
    {
        ASSERT(!this->options.uploadOnly);
        if (!this->getIsDirty())
        {
            return false;
        }

        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_COPY_DEST);

        for (const DirtyRange& range : this->dirtyRanges)
        {
            const uint32_t startBytes = sizeof(T) * range.begin;
            const uint32_t sizeBytes = sizeof(T) * (range.end - range.begin);
            cmdList->CopyBufferRegion(
                this->dev_buffer.Get(), startBytes, this->upload_buffer.Get(), startBytes, sizeBytes);
        }

        BufferHelper::stateTransitionResourceBarrier(cmdList,
                                                     this->dev_buffer.Get(),
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        this->setNotDirty();
        return true;
    }

    void resize(ToFreeList& toFreeList, uint32_t newSize)
    {
        uint32_t oldSize = this->size;
        T* host_oldBuffer = this->host_buffer;

        toFreeList.pushResource(this->upload_buffer, true);
        if (!this->options.uploadOnly)
        {
            toFreeList.pushResource(this->dev_buffer, false);
        }

        this->init(newSize, &toFreeList);

        const uint32_t copyCount = std::min(oldSize, newSize);
        memcpy(this->host_buffer, host_oldBuffer, sizeof(T) * copyCount);

        this->dirtyRanges.clear();
        this->insertDirtyRange(0, newSize);
    }

    inline void reset()
    {
        this->upload_buffer->Unmap(0, nullptr);
        this->upload_buffer.Reset();
        this->dev_buffer.Reset();
    }

    inline uint32_t getSize() const
    {
        return this->size;
    }

    inline bool getIsDirty() const
    {
        return !this->dirtyRanges.empty();
    }

    inline ID3D12Resource* getUploadBuffer() const
    {
        return this->upload_buffer.Get();
    }

    inline ID3D12Resource* getBuffer() const
    {
        ASSERT(!this->options.uploadOnly);
        return this->dev_buffer.Get();
    }

    inline D3D12_GPU_VIRTUAL_ADDRESS getGpuVirtualAddress() const
    {
        ASSERT(!this->options.uploadOnly);
        return this->dev_buffer->GetGPUVirtualAddress();
    }

    inline void setName(const std::wstring& name)
    {
        this->name = name;
    }
};
