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
