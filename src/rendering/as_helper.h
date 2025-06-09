#pragma once

#include <vector>

#include "dxr_common.h"

namespace AsHelper
{

struct GeometryWrapper
{
    ComPtr<ID3D12Resource> dev_vertUploadBuffer{nullptr};
    ComPtr<ID3D12Resource> dev_idxUploadBuffer{nullptr};

    ComPtr<ID3D12Resource> blas{nullptr};
};

GeometryWrapper makeBuffersAndBlas(ID3D12GraphicsCommandList4* cmdList,
                                   const std::vector<Vertex>* verts,
                                   const std::vector<uint32_t>* idx = nullptr);

ComPtr<ID3D12Resource> makeTLAS(ID3D12GraphicsCommandList4* cmdList,
                                ID3D12Resource* dev_instanceDescs,
                                uint32_t numInstances,
                                uint64_t* updateScratchSize);

}  // namespace AsHelper
