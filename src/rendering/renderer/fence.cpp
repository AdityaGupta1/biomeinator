// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "fence.h"

#include "rendering/dxr_common.h"
#include "rendering/renderer.h"
#include "logger.h"

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
        const DWORD waitResult = WaitForSingleObjectEx(this->fenceEvent, 1000 /*ms*/, true);
        if (waitResult == WAIT_TIMEOUT)
        {
            Logger::logError("GPU fence wait timed out (possible GPU hang), fence value: %llu", waitFenceValue);
            __debugbreak();
        }
        else if (waitResult == WAIT_FAILED)
        {
            Logger::logError("GPU fence wait failed, error: 0x%08X", GetLastError());
            __debugbreak();
        }
    }
}

void Fence::reset()
{
    this->fence.Reset();
    CloseHandle(this->fenceEvent);
}
