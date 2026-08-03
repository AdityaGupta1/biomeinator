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

    // nonzero only for BLASes built with allowUpdate
    size_t updateScratchSizeBytes{ 0 };

    // Stored here so BLAS refits reuse the same flags as the original build
    D3D12_RAYTRACING_GEOMETRY_FLAGS geometryFlags{ D3D12_RAYTRACING_GEOMETRY_FLAG_NONE };
};

struct BlasBuildInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };

    bool allowUpdate{ false };

    GeometryWrapper* outGeoWrapper{ nullptr };
};

void init();

void makeBlases(ID3D12GraphicsCommandList4* cmdList,
                ToFreeList& toFreeList,
                ManagedBuffer* dev_verts,
                ManagedBuffer* dev_idxs,
                const std::vector<BlasBuildInputs>& allInputs);

// In-place refit of BLASes originally built with allowUpdate, valid only if topology and vert count never change.
void updateBlases(ID3D12GraphicsCommandList4* cmdList,
                  ToFreeList& toFreeList,
                  const std::vector<GeometryWrapper*>& geoWrappers);

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
