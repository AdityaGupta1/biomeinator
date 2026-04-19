// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_common.h"
#include "rendering/common/common_structs.h"

#include <vector>

#include "managed_buffer.h"

class ToFreeList;

namespace AcsHelper
{

struct GeometryWrapper
{
    ManagedBufferSection blasBufferSection{};

    ManagedBufferSection vertsBufferSection{};
    ManagedBufferSection idxsBufferSection{};
};

struct BlasBuildInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };

    GeometryWrapper* outGeoWrapper{ nullptr };
};

void init();

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                ManagedBuffer* dev_verts,
                ManagedBuffer* dev_idxs,
                const std::vector<BlasBuildInputs>& allInputs);

struct TlasBuildInputs
{
    ID3D12Resource* dev_instanceDescs{ nullptr };
    uint32_t numInstances{ 0 };
    uint32_t* updateScratchSizePtr{ nullptr };

    ManagedBufferSection* outTlas{ nullptr };
};

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const TlasBuildInputs& inputs);

void reset();

}  // namespace AcsHelper
