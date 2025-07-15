#pragma once

#include "dxr_includes.h"
#include "common/common_structs.h"

class ParamBlockManager
{
private:
    ComPtr<ID3D12Resource> dev_paramBuffer{ nullptr };
    void* host_paramBuffer{ nullptr };

public:
    CameraParams* cameraParams{ nullptr };
    SceneParams* sceneParams{ nullptr };

    void init();

    ID3D12Resource* getDevBuffer() const;
};
