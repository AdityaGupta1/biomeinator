// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"
#include "common/common_params.h"

class ParamBlockManager
{
private:
    ComPtr<ID3D12Resource> dev_paramBuffer{ nullptr };
    void* host_paramBuffer{ nullptr };

public:
    HeapIndices* heapIndices{ nullptr };
    ConstantParams* constantParams{ nullptr };
    CameraParams* cameraParams{ nullptr };
    SceneParams* sceneParams{ nullptr };
    RenderParams* renderParams{ nullptr };
    RadianceCacheParams* rcParams{ nullptr };
    DebugParams* debugParams{ nullptr };

    void init();
    void reset();

    ID3D12Resource* getDevBuffer() const;

    void setName(const std::wstring& name);
};
