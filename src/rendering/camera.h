#pragma once

#include "common_structs.h"

class Camera
{
private:
    XMFLOAT3 right_WS;
    XMFLOAT3 up_WS;
    XMFLOAT3 forward_WS;
    const float tanHalfFovY;

public:
    Camera(float fovYRadians);

    void updateCameraParams(CameraParams* cameraParams);
};
