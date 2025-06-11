#pragma once

#include "rendering/dxr_common.h"

#include <vector>

#include "managed_buffer.h"
#include "to_free_list.h"

namespace AcsHelper
{

struct GeometryWrapper
{
    ComPtr<ID3D12Resource> dev_blas{ nullptr };

    ManagedBufferSection vertBufferSection{};
    ManagedBufferSection idxBufferSection{};
};

struct BlasBuildInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };

    ManagedBuffer* dev_verts{ nullptr };
    ManagedBuffer* dev_idxs{ nullptr };

    GeometryWrapper* outGeoWrapper;
};

void makeBlases(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, std::vector<BlasBuildInputs> allInputs);

struct TlasBuildInputs
{
    ID3D12Resource* dev_instanceDescs;
    uint32_t numInstances;
    uint64_t* updateScratchSizePtr;

    ComPtr<ID3D12Resource>* outTlas;
};

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList& toFreeList, TlasBuildInputs inputs);

}  // namespace AcsHelper
