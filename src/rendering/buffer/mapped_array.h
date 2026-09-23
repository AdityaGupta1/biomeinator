// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/buffer/buffer_helper.h"
#include "rendering/buffer/dirty_range_set.h"
#include "rendering/buffer/gpu_memory_reporter.h"
#include "rendering/buffer/to_free_list.h"
#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "util/util.h"

#include "debug.h"

#include <algorithm>
#include <cstring>
#include <vector>

struct MappedArrayOptions
{
    bool uploadOnly{ false }; // only allocate upload_buffer; dev_buffer is unused
    // Stage through one upload buffer per frame in flight. Without this the CPU writes the
    // single mapped upload buffer for frame N+1 while frame N's not-yet-executed copy still
    // has to read it, so the dev buffer receives a mix of two frames' data.
    //
    // Only valid for arrays that rewrite every live element on each dirty cycle. An array
    // updated element-wise via markDirty would find stale values in the slot it lands on.
    bool perFrameUpload{ false };
};

template<typename T> class MappedArray : public GpuMemoryReporter
{
private:
    std::wstring name{ L"MappedArray" };

    MappedArrayOptions options{};

    uint32_t size{ 0 };
    std::vector<T*> host_buffers;
    std::vector<ComPtr<ID3D12Resource>> upload_buffers;
    // Single regardless of perFrameUpload: the GPU is its only accessor, and the copy's
    // state transitions already order the write against the shader read.
    ComPtr<ID3D12Resource> dev_buffer{ nullptr };

    uint32_t getUploadSlotIdx() const
    {
        const uint32_t slotIdx = this->options.perFrameUpload ? Renderer::getFrameIndex() : 0;
        ASSERT(slotIdx < this->upload_buffers.size());
        return slotIdx;
    }

    DirtyRangeSet dirtyRanges;

    void setNotDirty()
    {
        this->dirtyRanges.clear();
    }

    void init(uint32_t size, ToFreeList* toFreeList)
    {
        this->size = size;
        const uint32_t sizeBytes = sizeof(T) * size;

        const std::wstring sizeStr = L"(size = " + std::to_wstring(sizeBytes) + L" bytes) ";

        this->upload_buffers.resize(this->options.perFrameUpload ? Renderer::NUM_FRAMES_IN_FLIGHT : 1);
        this->host_buffers.resize(this->upload_buffers.size());

        for (size_t slotIdx = 0; slotIdx < this->upload_buffers.size(); ++slotIdx)
        {
            this->upload_buffers[slotIdx] = BufferHelper::createBasicBuffer(sizeBytes, &UPLOAD_HEAP);
            this->upload_buffers[slotIdx]->Map(0, nullptr, reinterpret_cast<void**>(&this->host_buffers[slotIdx]));

            const std::wstring slotStr =
                this->options.perFrameUpload ? (L" " + std::to_wstring(slotIdx)) : std::wstring();
            const std::wstring uploadBufferNameWithSize = this->name + L" upload_buffer" + slotStr + sizeStr;
            this->upload_buffers[slotIdx]->SetName(uploadBufferNameWithSize.c_str());
        }

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

    inline void init(uint32_t size, MappedArrayOptions options)
    {
        this->options = options;
        this->init(size, nullptr);
    }

    GpuMemoryEntry reportGpuMemory() const override
    {
        const size_t numBuffers = this->upload_buffers.size() + (this->dev_buffer != nullptr ? 1 : 0);
        const size_t allocatedBytes = sizeof(T) * this->size * numBuffers;
        return {
            .name = Util::to_string(this->name.c_str()),
            .allocatedBytes = allocatedBytes,
            .usedBytes = allocatedBytes,
        };
    }

    T& operator[](uint32_t idx)
    {
        ASSERT(idx < this->size);
        return this->host_buffers[this->getUploadSlotIdx()][idx];
    }

    inline void markDirty(uint32_t idx)
    {
        const bool inBounds = idx < this->size;
        ASSERT(inBounds, "MappedArray dirty index is out of bounds");
        if (inBounds)
        {
            this->dirtyRanges.insert(idx, idx + 1);
        }
    }

    inline void markDirtyRange(uint32_t begin, uint32_t end)
    {
        const bool inBounds = begin <= end && end <= this->size;
        ASSERT(inBounds, "MappedArray dirty range is out of bounds");
        if (inBounds)
        {
            this->dirtyRanges.insert(begin, end);
        }
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

        ID3D12Resource* const upload_buffer = this->upload_buffers[this->getUploadSlotIdx()].Get();
        for (const DirtyRange& range : this->dirtyRanges.getRanges())
        {
            const uint32_t startBytes = sizeof(T) * range.begin;
            const uint32_t sizeBytes = sizeof(T) * (range.end - range.begin);
            cmdList->CopyBufferRegion(this->dev_buffer.Get(), startBytes, upload_buffer, startBytes, sizeBytes);
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
        const uint32_t oldSize = this->size;
        // Only the current slot's contents need carrying over; the others are rewritten in
        // full before they are next read, which is the precondition for perFrameUpload.
        const uint32_t slotIdx = this->getUploadSlotIdx();
        T* const host_oldBuffer = this->host_buffers[slotIdx];

        for (const ComPtr<ID3D12Resource>& upload_buffer : this->upload_buffers)
        {
            toFreeList.pushResource(upload_buffer);
        }
        if (!this->options.uploadOnly)
        {
            toFreeList.pushResource(this->dev_buffer);
        }

        this->init(newSize, &toFreeList);

        const uint32_t copyCount = std::min(oldSize, newSize);
        memcpy(this->host_buffers[slotIdx], host_oldBuffer, sizeof(T) * copyCount);

        this->dirtyRanges.clear();
        this->dirtyRanges.insert(0, newSize);
    }

    // For arrays whose device buffer is the source of truth (filled incrementally or by the
    // GPU): the old device contents are copied over on the GPU and nothing is marked dirty, so
    // the stale staging slots are never uploaded. Only valid for device-backed arrays.
    void resizeOnDevice(ID3D12GraphicsCommandList* cmdList, ToFreeList& toFreeList, uint32_t newSize)
    {
        ASSERT(!this->options.uploadOnly);
        ASSERT(this->dirtyRanges.empty(), "resizeOnDevice with staged writes pending");
        const uint32_t oldSize = this->size;
        ID3D12Resource* const dev_oldBuffer = toFreeList.pushResource(this->dev_buffer);
        for (const ComPtr<ID3D12Resource>& upload_buffer : this->upload_buffers)
        {
            toFreeList.pushResource(upload_buffer);
        }

        this->init(newSize, &toFreeList);

        BufferHelper::copyBufferRegion(cmdList,
                                       this->dev_buffer.Get(),
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       0,
                                       dev_oldBuffer,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       0,
                                       sizeof(T) * std::min(oldSize, newSize));
    }

    inline void reset()
    {
        for (ComPtr<ID3D12Resource>& upload_buffer : this->upload_buffers)
        {
            upload_buffer->Unmap(0, nullptr);
        }
        this->upload_buffers.clear();
        this->host_buffers.clear();

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
        return this->upload_buffers.empty() ? nullptr : this->upload_buffers[this->getUploadSlotIdx()].Get();
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
