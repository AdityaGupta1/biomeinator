/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
#else
#define CHECK_HRESULT(expr) (expr)
#endif

#define CHECK_SLANG_DIAGNOSTICS(blob)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((blob))                                                                                                    \
        {                                                                                                              \
            fprintf(stderr, "Slang diagnostics: %s\n", (const char*)(blob)->getBufferPointer());                       \
        }                                                                                                              \
    } while (0)

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
