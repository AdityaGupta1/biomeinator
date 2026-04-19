// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "fence.h"

#include "dxr_common.h"
#include "renderer.h"

void Fence::init()
{
    CHECK_HRESULT(Renderer::getDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&this->fence)));
    this->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

uint64_t Fence::signal(ID3D12CommandQueue* cmdQueue)
{
    const uint64_t signalFenceValue = this->fenceValue++;
    CHECK_HRESULT(cmdQueue->Signal(this->fence.Get(), signalFenceValue));
    return signalFenceValue;
}

void Fence::waitFor(uint64_t waitFenceValue)
{
    if (this->fence->GetCompletedValue() < waitFenceValue)
    {
        CHECK_HRESULT(fence->SetEventOnCompletion(waitFenceValue, this->fenceEvent));
        WaitForSingleObjectEx(this->fenceEvent, 1000 /*ms*/, true);
    }
}

void Fence::reset()
{
    this->fence.Reset();
    CloseHandle(this->fenceEvent);
}
