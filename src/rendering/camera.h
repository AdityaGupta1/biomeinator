#pragma once

#include "common_structs.h"
#include "dxr_includes.h"

class Camera
{
private:
    CameraParams* host_cameraParams{ nullptr };
    ComPtr<ID3D12Resource> dev_cameraParams{ nullptr };

public:
    void init(float fovYRadians);

    void moveLinear(XMFLOAT3 linearMovement);

    ID3D12Resource* getCameraParamsBuffer();
};
