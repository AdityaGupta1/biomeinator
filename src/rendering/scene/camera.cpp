#include "camera.h"

#include "rendering/dxr_common.h"

#include <numbers>

using namespace DirectX;

void Camera::init(float defaultFovYRadians)
{
    this->params.pos_WS = { 0, 1.5f, -7.f };

    this->defaultFovYRadians = this->currentFovYRadians = defaultFovYRadians;
    this->params.tanHalfFovY = tanf(this->currentFovYRadians * 0.5f);

    this->setDirectionVectorsFromAngles();
}

void Camera::setDirectionVectorsFromAngles()
{
    const float cosPhi = cosf(phi);
    XMVECTOR forward = XMVectorSet(cosPhi * sinf(theta), sinf(phi), cosPhi * cosf(theta), 0.0f);
    forward = XMVector3Normalize(forward);

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

    up = XMVector3Normalize(XMVector3Cross(forward, right));

    XMStoreFloat3(&this->params.forward_WS, forward);
    XMStoreFloat3(&this->params.right_WS, right);
    XMStoreFloat3(&this->params.up_WS, up);
}

void Camera::moveLinear(XMFLOAT3 linearMovement)
{
    const XMVECTOR rightFlat_WS = XMLoadFloat3(&this->params.right_WS); // already flat
    const XMVECTOR forwardFlat_WS =
        XMVector3Normalize(XMVectorSet(this->params.forward_WS.x, 0, this->params.forward_WS.z, 0));

    const XMVECTOR displacement = XMVectorScale(rightFlat_WS, linearMovement.x) +
                                  XMVectorSet(0, linearMovement.y, 0, 0) +
                                  XMVectorScale(forwardFlat_WS, linearMovement.z);

    XMVECTOR pos = XMLoadFloat3(&this->params.pos_WS);
    pos = XMVectorAdd(pos, displacement);
    XMStoreFloat3(&this->params.pos_WS, pos);
}

constexpr float absMaxPhi = std::numbers::pi / 2.f - 0.01f; // slightly under pi/2 to avoid going past the poles

void Camera::rotate(float dTheta, float dPhi)
{
    this->theta += dTheta;
    this->phi = fmaxf(-absMaxPhi, fminf(absMaxPhi, this->phi - dPhi));
    this->setDirectionVectorsFromAngles();
}

constexpr float playerHorizontalSpeed = 11.0f;
constexpr float playerVerticalSpeed = 7.0f;
constexpr XMFLOAT3 playerLinearSpeed = XMFLOAT3(playerHorizontalSpeed, playerVerticalSpeed, playerHorizontalSpeed);

constexpr float mouseSensitivity = 0.0016f;

constexpr float fovTransitionSpeed = 10.f;
constexpr float zoomFovRatio = 0.3f;

void Camera::processPlayerInput(const PlayerInput& input, double deltaTime)
{
    if (input.linearInput.x != 0 || input.linearInput.y != 0 || input.linearInput.z != 0)
    {
        XMVECTOR linearSpeed = XMLoadFloat3(&playerLinearSpeed);
        linearSpeed = XMVectorScale(linearSpeed, static_cast<float>(deltaTime) * input.linearSpeedMultiplier);
        const XMVECTOR linearMovement = XMVectorMultiply(linearSpeed, XMLoadFloat3(&input.linearInput));
        XMFLOAT3 storedLinearMovement;
        XMStoreFloat3(&storedLinearMovement, linearMovement);
        this->moveLinear(storedLinearMovement);
    }

    if (input.mouseMovement.x != 0 || input.mouseMovement.y != 0)
    {
        this->rotate(input.mouseMovement.x * mouseSensitivity, input.mouseMovement.y * mouseSensitivity);
    }

    const float targetFov = input.isZoomHeld ? this->defaultFovYRadians * zoomFovRatio : this->defaultFovYRadians;
    const float deltaFov = targetFov - this->currentFovYRadians;
    const float maxStep = fovTransitionSpeed * fabsf(deltaFov) * static_cast<float>(deltaTime);
    if (fabsf(deltaFov) <= maxStep)
    {
        this->currentFovYRadians = targetFov;
    }
    else
    {
        this->currentFovYRadians += (deltaFov > 0 ? maxStep : -maxStep);
    }

    if (fabsf(deltaFov) > 0.f)
    {
        this->params.tanHalfFovY = tanf(this->currentFovYRadians * 0.5f);
    }
}

void Camera::copyParamsTo(CameraParams* dest) const
{
    memcpy(dest, &this->params, sizeof(CameraParams));
}
