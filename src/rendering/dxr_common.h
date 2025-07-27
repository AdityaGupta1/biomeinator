#pragma once

#include "dxr_includes.h"
#include "common/common_structs.h"

#ifdef _DEBUG
#include <stdio.h>
#define CHECK_HRESULT(expr)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        HRESULT _hr = (expr);                                                                                          \
        if (FAILED(_hr))                                                                                               \
        {                                                                                                              \
            fprintf(stderr, "HRESULT failed: %s (0x%08X)\n", #expr, static_cast<unsigned int>(_hr));                   \
            __debugbreak();                                                                                            \
        }                                                                                                              \
    } while (0)

#define CHECK_SLANG_DIAGNOSTICS(blob)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((blob))                                                                                                    \
        {                                                                                                              \
            fprintf(stderr, "Slang diagnostics: %s\n", (const char*)(blob)->getBufferPointer());                       \
        }                                                                                                              \
    } while (0)
#else
#define CHECK_HRESULT(expr) (expr)
#define CHECK_SLANG_DIAGNOSTICS(blob) ((void)0)
#endif

constexpr DXGI_SAMPLE_DESC NO_AA = {
    .Count = 1,
    .Quality = 0
};

constexpr D3D12_HEAP_PROPERTIES UPLOAD_HEAP = {
    .Type = D3D12_HEAP_TYPE_UPLOAD,
};

constexpr D3D12_HEAP_PROPERTIES DEFAULT_HEAP = {
    .Type = D3D12_HEAP_TYPE_DEFAULT,
};

constexpr D3D12_HEAP_PROPERTIES READBACK_HEAP = {
    .Type = D3D12_HEAP_TYPE_READBACK,
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
