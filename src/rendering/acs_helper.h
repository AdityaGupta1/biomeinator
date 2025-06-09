#pragma once

#include <vector>

#include "dxr_common.h"
#include "managed_buffer.h"

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

    ManagedBuffer* dev_managedVertBuffer{ nullptr };
    ManagedBuffer* dev_managedIdxBuffer{ nullptr };

    GeometryWrapper* outGeoWrapper;
};

void makeBuffersAndBlases(ID3D12GraphicsCommandList4* cmdList,
                          ToFreeList* toFreeList,
                          std::vector<BlasBuildInputs> allInputs);

struct TlasBuildInputs
{
    ID3D12Resource* dev_instanceDescs;
    uint32_t numInstances;
    uint64_t* updateScratchSizePtr;

    ComPtr<ID3D12Resource>* outTlas;
};

void makeTlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList* toFreeList, TlasBuildInputs inputs);

}  // namespace AcsHelper
