#include "camera.h"

#include "dxr_common.h"
#include "buffer/buffer_helper.h"

#include <numbers>

void Camera::init(float fovYRadians)
{
    dev_cameraParams = BufferHelper::createBasicBuffer(
        sizeof(CameraParams), &UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

    dev_cameraParams->Map(0, nullptr, reinterpret_cast<void**>(&this->host_cameraParams));

    this->host_cameraParams->pos_WS = { 0, 1.5, -7 };
    this->host_cameraParams->tanHalfFovY = tanf(fovYRadians * 0.5f);

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

    XMStoreFloat3(&this->host_cameraParams->forward_WS, forward);
    XMStoreFloat3(&this->host_cameraParams->right_WS, right);
    XMStoreFloat3(&this->host_cameraParams->up_WS, up);
}

void Camera::moveLinear(XMFLOAT3 linearMovement)
{
    const XMVECTOR rightFlat_WS = XMLoadFloat3(&this->host_cameraParams->right_WS); // already flat
    const XMVECTOR forwardFlat_WS = XMVector3Normalize(
        XMVectorSet(this->host_cameraParams->forward_WS.x, 0, this->host_cameraParams->forward_WS.z, 0));

    const XMVECTOR displacement = XMVectorScale(rightFlat_WS, linearMovement.x) +
                                  XMVectorSet(0, linearMovement.y, 0, 0) +
                                  XMVectorScale(forwardFlat_WS, linearMovement.z);

    XMVECTOR pos = XMLoadFloat3(&this->host_cameraParams->pos_WS);
    pos = XMVectorAdd(pos, displacement);
    XMStoreFloat3(&this->host_cameraParams->pos_WS, pos);
}

constexpr float absMaxPhi = std::numbers::pi / 2.f - 0.01f; // slightly under pi/2 to avoid going past the poles

void Camera::rotate(float dTheta, float dPhi)
{
    this->theta += dTheta;
    this->phi = fmaxf(-absMaxPhi, fminf(absMaxPhi, this->phi - dPhi));
    this->setDirectionVectorsFromAngles();
}

ID3D12Resource* Camera::getCameraParamsBuffer()
{
    return dev_cameraParams.Get();
}
