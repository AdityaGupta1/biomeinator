// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "managed_buffer.h"

#include "debug.h"

#include <future>
#include <vector>

class ReservedManagedBuffer final : public ManagedBuffer
{
public:
    ReservedManagedBuffer(size_t maxReservedSizeBytes,
                          D3D12_RESOURCE_STATES initialResourceState,
                          ManagedBufferOptions options);

protected:
    void initializeStorage(ToFreeList* toFreeList, size_t sizeBytes) override;

    void ensureCapacity(ID3D12GraphicsCommandList* cmdList, ToFreeList& toFreeList, size_t minCapacityBytes) override;

    void onReset() override;

private:
    const size_t maxReservedSizeBytes; // virtual size

    std::vector<ComPtr<ID3D12Heap>> heaps;

    // The heap after the one just mapped is created on a background thread, since CreateHeap
    // takes tens of milliseconds and would otherwise land inside a streaming frame
    std::future<ComPtr<ID3D12Heap>> prefetchedHeap;
    size_t prefetchedHeapSizeBytes{ 0 };

    size_t mapNewHeap(size_t virtualStartTile, size_t minAdditionalBytes, bool prefetchNext);
};
