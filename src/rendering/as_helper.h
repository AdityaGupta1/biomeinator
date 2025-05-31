#pragma once

#include <vector>

#include "dxr_common.h"

namespace AsHelper
{

struct BlasWrapper
{
    ComPtr<ID3D12Resource> vertBuffer{nullptr};
    ComPtr<ID3D12Resource> idxBuffer{nullptr};

    ComPtr<ID3D12Resource> blas{nullptr};
};

BlasWrapper initBuffersAndBlas(const std::vector<Vertex>* verts, const std::vector<uint32_t>* idx = nullptr);

ComPtr<ID3D12Resource> makeTLAS(ID3D12Resource* dev_instanceDescs, uint32_t numInstances, uint64_t* updateScratchSize);

}  // namespace AsHelper
