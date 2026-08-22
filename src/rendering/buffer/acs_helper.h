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
    // R16 per-triangle OMM index buffer; a valid section makes the geometry OMM_TRIANGLES
    ManagedBufferSection ommIdxsBufferSection{};

    // nonzero only for BLASes built with allowUpdate
    size_t updateScratchSizeBytes{ 0 };

    // Stored here so BLAS refits reuse the same flags as the original build
    D3D12_RAYTRACING_GEOMETRY_FLAGS geometryFlags{ D3D12_RAYTRACING_GEOMETRY_FLAG_NONE };
};

struct BlasBuildInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };
    // Per-triangle OMM Array indices (or special indices); requires a built OMM Array
    const std::vector<uint16_t>* host_ommIdxs{ nullptr };

    bool allowUpdate{ false };
    // Marks the whole geometry opaque so traversal never invokes anyhit for it
    bool isOpaque{ false };

    GeometryWrapper* outGeoWrapper{ nullptr };
};

void init();

struct OmmArrayBuildInputs
{
    // Raw OC1 bitmask data, indexed by the descs' ByteOffset
    const std::vector<uint8_t>* host_ommData{ nullptr };
    const std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_DESC>* host_ommDescs{ nullptr };
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY histogram{};

    ManagedBufferSection* outOmmArray{ nullptr };
};

// Builds an OMM Array into the shared AS buffer and issues a UAV barrier so later BLAS builds
// can reference it. The array's GPU VA is retained for OMM-linked BLAS builds (makeBlases
// inputs with host_ommIdxs), which must therefore come after the single buildOmmArray call.
void buildOmmArray(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, const OmmArrayBuildInputs& inputs);

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
