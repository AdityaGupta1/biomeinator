#pragma once

#include "dxr_includes.h"
#include "managed_buffer.h"

class Instance
{
    ComPtr<ID3D12Resource> vertBuffer{ nullptr };
    ComPtr<ID3D12Resource> idxBuffer{ nullptr };

    ComPtr<ID3D12Resource> blas{ nullptr };
};
