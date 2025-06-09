#pragma once

#include <vector>

#include "dxr_common.h"
#include "managed_buffer.h"

namespace AsHelper
{

struct GeometryWrapper
{
    ComPtr<ID3D12Resource> dev_blas{ nullptr };

    ManagedBufferSection vertBufferSection{};
    ManagedBufferSection idxBufferSection{};
};

struct BlasInputs
{
    const std::vector<Vertex>* host_verts{ nullptr };
    const std::vector<uint32_t>* host_idxs{ nullptr };

    ManagedBuffer* dev_managedVertBuffer{ nullptr };
    ManagedBuffer* dev_managedIdxBuffer{ nullptr };

    GeometryWrapper* outGeoWrapper;
};

void makeBuffersAndBlas(ID3D12GraphicsCommandList4* cmdList, ToFreeList* toFreeList, BlasInputs inputs);

ComPtr<ID3D12Resource> makeTLAS(ID3D12GraphicsCommandList4* cmdList,
                                ToFreeList* toFreeList,
                                ID3D12Resource* dev_instanceDescs,
                                uint32_t numInstances,
                                uint64_t* updateScratchSize);

}  // namespace AsHelper
