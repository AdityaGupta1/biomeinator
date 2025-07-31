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

#include <vector>

#include "managed_buffer.h"

class ToFreeList;

namespace AcsHelper
{

struct GeometryWrapper
{
    ComPtr<ID3D12Resource> dev_blas{ nullptr };

    ManagedBufferSection vertsBufferSection{};
    ManagedBufferSection idxsBufferSection{};
};

struct BlasBuildInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };

    ManagedBuffer* dev_verts{ nullptr };
    ManagedBuffer* dev_idxs{ nullptr };

    GeometryWrapper* outGeoWrapper{ nullptr };
};

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                const std::vector<BlasBuildInputs>& allInputs);

struct TlasBuildInputs
{
    ID3D12Resource* dev_instanceDescs{ nullptr };
    uint32_t numInstances{ 0 };
    uint32_t* updateScratchSizePtr{ nullptr };

    ComPtr<ID3D12Resource>* outTlas{ nullptr };
};

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const TlasBuildInputs& inputs);

}  // namespace AcsHelper
