#pragma once

#include "dxr_includes.h"

#include "common_structs.h"
#include "host_structs.h"

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

    void moveLinear(DirectX::XMFLOAT3 linearMovement);
    void rotate(float dTheta, float dPhi);

    void processPlayerInput(const PlayerInput& input, double deltaTime);

    ID3D12Resource* getCameraParamsBuffer();
};
