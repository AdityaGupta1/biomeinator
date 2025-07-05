#pragma once

#include "rendering/common_structs.h"
#include "rendering/dxr_includes.h"
#include "rendering/host_structs.h"

#include <numbers>

class Camera
{
private:
    CameraParams params{};

    float phi{ 0 };
    float theta{ std::numbers::pi_v<float> };

    float defaultFovYRadians{ 0 };
    float currentFovYRadians{ 0 };

    void setDirectionVectorsFromAngles();

    void moveLinear(DirectX::XMFLOAT3 linearMovement);
    void rotate(float dTheta, float dPhi);

public:
    void init(float defaultFovYRadians);

    void copyParamsTo(CameraParams* dest) const;

    void processPlayerInput(const PlayerInput& input, double deltaTime);
};
