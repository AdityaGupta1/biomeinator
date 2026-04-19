// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "managed_buffer.h"

class CommittedManagedBuffer final : public ManagedBuffer
{
public:
    CommittedManagedBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
                           D3D12_RESOURCE_STATES initialResourceState,
                           ManagedBufferOptions options);

protected:
    void initializeStorage(ToFreeList* toFreeList, size_t sizeBytes) override;

    void ensureCapacity(ID3D12GraphicsCommandList* cmdList,
                        ToFreeList& toFreeList,
                        size_t minCapacityBytes,
                        bool useBackFreeSection) override;

    void onReset() override;
};
