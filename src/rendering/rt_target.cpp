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

#include "rt_target.h"

#include "dxr_common.h"
#include "renderer.h"
#include "buffer/buffer_helper.h"

#include "debug.h"

RtTarget::RtTarget(const std::wstring& name,
                   DXGI_FORMAT format,
                   uint32_t debugOutputNumChannels,
                   bool isFullSize,
                   bool hasUav,
                   bool hasSrv)
    : name(name), debugOutputNumChannels(debugOutputNumChannels), isFullSize(isFullSize), hasUav(hasUav), hasSrv(hasSrv)
{
    D3D12_RESOURCE_FLAGS targetResourceFlags = D3D12_RESOURCE_FLAG_NONE;
    if (this->hasUav)
    {
        targetResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (!this->hasSrv)
    {
        targetResourceFlags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE; // untested
    }

    this->targetResourceDesc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = static_cast<uint32_t>(-1),
        .Height = static_cast<uint32_t>(-1),
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = format,
        .SampleDesc = SAMPLE_DESC_NO_AA,
        .Flags = targetResourceFlags,
    };

    if (this->hasUav)
    {
        this->uav.desc = {
            .Format = format,
            .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        };
    }

    if (this->hasSrv)
    {
        this->srv.desc = BASIC_SRV_DESC;
        this->srv.desc.Format = format;
        this->srv.desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        this->srv.desc.Texture2D = {
            .MostDetailedMip = 0,
            .MipLevels = static_cast<uint32_t>(-1),
            .PlaneSlice = 0,
        };
    }
}

void RtTarget::setDimensions(uint32_t width, uint32_t height)
{
    this->targetResourceDesc.Width = width;
    this->targetResourceDesc.Height = height;
}

void RtTarget::init()
{
    this->targetResourceState = D3D12_RESOURCE_STATE_COMMON;
    Renderer::getDevice()->CreateCommittedResource(&DEFAULT_HEAP,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &this->targetResourceDesc,
                                                   this->targetResourceState,
                                                   nullptr,
                                                   IID_PPV_ARGS(&this->target));
    this->target->SetName(this->name.c_str());

    if (this->hasUav)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE uavHandle;
        this->uav.idx = Renderer::sharedDescHeapAlloc.alloc(&uavHandle);
        Renderer::getDevice()->CreateUnorderedAccessView(this->target.Get(), nullptr, &this->uav.desc, uavHandle);
    }

    if (this->hasSrv)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle;
        this->srv.idx = Renderer::sharedDescHeapAlloc.alloc(&srvHandle);
        Renderer::getDevice()->CreateShaderResourceView(this->target.Get(), &this->srv.desc, srvHandle);
    }
}

// This function doesn't use a toFreeList since it only gets called during resizes, which call Renderer::flush() anyway.
void RtTarget::reset()
{
    if (this->uav.idx != ~0u)
    {
        Renderer::sharedDescHeapAlloc.free(this->uav.idx);
        this->uav.idx = ~0u;
    }

    if (this->srv.idx != ~0u)
    {
        Renderer::sharedDescHeapAlloc.free(this->srv.idx);
        this->srv.idx = ~0u;
    }

    if (this->target)
    {
        this->target.Reset();
    }
}

// not sure how this works over frame boundaries but I assume it's fine because of promotion and decay?
void RtTarget::transitionToState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState)
{
    if (newState != this->targetResourceState)
    {
        BufferHelper::stateTransitionResourceBarrier(cmdList, this->target.Get(), this->targetResourceState, newState);
        this->targetResourceState = newState;
    }
}

ID3D12Resource* RtTarget::getTarget() const
{
    return this->target.Get();
}

uint32_t RtTarget::getUavIdx() const
{
    ASSERT(this->uav.idx != ~0u);
    return this->uav.idx;
}

uint32_t RtTarget::getSrvIdx() const
{
    ASSERT(this->srv.idx != ~0u);
    return this->srv.idx;
}
