#pragma once

#include "dxr_includes.h"

#include "common_structs.h"
#include "host_structs.h"

class Camera
{
private:
    CameraParams params{};

    float phi{ 0 };
    float theta{ 0 };

    float defaultFovYRadians{ 0 };
    float currentFovYRadians{ 0 };

    void setDirectionVectorsFromAngles();

    void moveLinear(DirectX::XMFLOAT3 linearMovement);
    void rotate(float dTheta, float dPhi);

public:
    void init(float fovYRadians);

    void copyTo(CameraParams* dest) const;

    void processPlayerInput(const PlayerInput& input, double deltaTime);
};
