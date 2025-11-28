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

#include "dxr_includes.h"

#include <string>

struct RtTarget
{
private:
    const std::wstring name;

    ComPtr<ID3D12Resource> target{ nullptr };
    D3D12_RESOURCE_DESC targetResourceDesc{};
    D3D12_RESOURCE_STATES targetResourceState{ D3D12_RESOURCE_STATE_COMMON };

    struct
    {
        uint32_t idx{ ~0u };
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    } uav;

    struct
    {
        uint32_t idx{ ~0u };
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    } srv;

public:
    const uint32_t debugOutputNumChannels;

    const bool isFullSize;

    const bool hasUav;
    const bool hasSrv;

    RtTarget(const std::wstring& name,
             DXGI_FORMAT format,
             uint32_t debugOutputNumChannels = 0,
             bool isFullSize = false,
             bool hasUav = true,
             bool hasSrv = true);

    void setDimensions(uint32_t width, uint32_t height);

    void init();
    void reset();

    void transitionToState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

    ID3D12Resource* getTarget() const;

    uint32_t getUavIdx() const;
    uint32_t getSrvIdx() const;
};
