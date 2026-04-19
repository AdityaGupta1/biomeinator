// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"

class Fence
{
private:
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue{ 0 };
    HANDLE fenceEvent{ nullptr };

public:
    void init();

    uint64_t signal(ID3D12CommandQueue* cmdQueue);
    void waitFor(uint64_t waitFenceValue);

    void reset();
};
