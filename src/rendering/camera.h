#pragma once

#include "common_structs.h"
#include "dxr_includes.h"

class Camera
{
private:
    CameraParams* host_cameraParams{ nullptr };
    ComPtr<ID3D12Resource> dev_cameraParams{ nullptr };

    float phi{ 0 };
    float theta{ 0 };

    void setDirectionVectorsFromAngles();

public:
    void init(float fovYRadians);

    void moveLinear(XMFLOAT3 linearMovement);
    void rotate(float dTheta, float dPhi);

    ID3D12Resource* getCameraParamsBuffer();
};
