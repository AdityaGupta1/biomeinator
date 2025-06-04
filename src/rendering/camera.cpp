#include "camera.h"

#include "dxr_common.h"
#include "buffer_helper.h"

void Camera::init(float fovYRadians)
{
    dev_cameraParams = BufferHelper::createBasicBuffer(
        sizeof(CameraParams), &UPLOAD_HEAP, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

    dev_cameraParams->Map(0, nullptr, reinterpret_cast<void**>(&this->host_cameraParams));

    this->host_cameraParams->pos_WS = { 0, 1.5, -7 };
    this->host_cameraParams->right_WS = { 1, 0, 0 };
    this->host_cameraParams->up_WS = { 0, 1, 0 };
    this->host_cameraParams->forward_WS = { 0, 0, 1 };
    this->host_cameraParams->tanHalfFovY = tanf(fovYRadians * 0.5f);
}

void Camera::moveLinear(XMFLOAT3 linearMovement)
{
    const XMVECTOR right = XMLoadFloat3(&host_cameraParams->right_WS);
    const XMVECTOR up = XMLoadFloat3(&host_cameraParams->up_WS);
    const XMVECTOR forward = XMLoadFloat3(&host_cameraParams->forward_WS);

    const XMVECTOR displacement = XMVectorScale(right, linearMovement.x) + XMVectorScale(up, linearMovement.y) +
                            XMVectorScale(forward, linearMovement.z);

    XMVECTOR pos = XMLoadFloat3(&host_cameraParams->pos_WS);
    pos = XMVectorAdd(pos, displacement);
    XMStoreFloat3(&host_cameraParams->pos_WS, pos);
}

ID3D12Resource* Camera::getCameraParamsBuffer()
{
    return dev_cameraParams.Get();
}
