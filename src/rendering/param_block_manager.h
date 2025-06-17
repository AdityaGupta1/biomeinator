#pragma once

#include "buffer/buffer_helper.h"
#include "common_structs.h"
#include "dxr_includes.h"

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
