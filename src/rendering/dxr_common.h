#pragma once

#include "dxr_includes.h"
#include "common_structs.h"

constexpr DXGI_SAMPLE_DESC NO_AA = {
    .Count = 1,
    .Quality = 0
};

constexpr D3D12_HEAP_PROPERTIES UPLOAD_HEAP = {
    .Type = D3D12_HEAP_TYPE_UPLOAD
};

constexpr D3D12_HEAP_PROPERTIES DEFAULT_HEAP = {
    .Type = D3D12_HEAP_TYPE_DEFAULT
};

constexpr D3D12_RESOURCE_DESC BASIC_BUFFER_DESC = {
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Width = 0, // will be changed in copies
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .SampleDesc = NO_AA,
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE,
};
