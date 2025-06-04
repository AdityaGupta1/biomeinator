#include "camera.h"

Camera::Camera(float fovYRadians)
    : right_WS(1, 0, 0), up_WS(0, 1, 0), forward_WS(0, 0, 1), tanHalfFovY(tanf(fovYRadians * 0.5f))
{}

void Camera::updateCameraParams(CameraParams* cameraParams)
{
    cameraParams->right_WS = this->right_WS;
    cameraParams->up_WS = this->up_WS;
    cameraParams->forward_WS = this->forward_WS;
    cameraParams->tanHalfFovY = this->tanHalfFovY;
}
