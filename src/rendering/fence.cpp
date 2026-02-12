/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2026 Aditya Gupta

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
