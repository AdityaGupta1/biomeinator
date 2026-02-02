/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "camera.h"

#include "dxr_common.h"
#include "renderer.h"
#include "scene/scene.h"
#include "settings_manager.h"

#include <numbers>

using namespace DirectX;

void Camera::init(float defaultFovYRadians)
{
    if (SettingsManager::getAsBool("voxelMode"))
    {
        this->setPos_WS({ 0, 196.f, 0 });
    }
    else
    {
        this->setPos_WS({ 0, 1.5f, 7.f });
    }

    this->defaultFovYRadians = this->currentFovYRadians = defaultFovYRadians;
    this->params.tanHalfFovY = tanf(this->currentFovYRadians * 0.5f);

    this->setDirectionVectorsFromAngles();

    this->params.nearPlane = 0.1f;
    this->params.farPlane = 10000.f;

    XMMATRIX identity = XMMatrixIdentity();
    XMStoreFloat4x4(&this->worldToPrevViewMat, identity);
    XMStoreFloat4x4(&this->prevViewToPrevClipMat, identity);
}

void Camera::setJitterHaltonSequenceLength(uint32_t sequenceLength)
{
    this->jitterHalton.init(sequenceLength);
}

void Camera::setDirectionVectorsFromAngles()
{
    const float cosPhi = cosf(phi);
    const XMVECTOR forward =
        XMVector3Normalize(XMVectorSet(cosPhi * sinf(theta), sinf(phi), cosPhi * cosf(theta), 0.0f));

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, up));
    up = XMVector3Normalize(XMVector3Cross(right, forward));

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

    XMFLOAT3 displacementVec;
    XMStoreFloat3(&displacementVec, displacement);
    this->posFloat_WS += glm::vec3(displacementVec.x, displacementVec.y, displacementVec.z);
}

constexpr float absMaxPhi = std::numbers::pi_v<float> / 2.f - 0.01f; // slightly under pi/2 to avoid going past the poles

void Camera::rotate(float dTheta, float dPhi)
{
    this->theta -= dTheta;
    this->phi = fmaxf(-absMaxPhi, fminf(absMaxPhi, this->phi - dPhi));
    this->setDirectionVectorsFromAngles();
}

void Camera::setMatrices(bool instanceOffsetChanged)
{
    const XMVECTOR eye = XMLoadFloat3(&this->params.pos_WS);
    const XMVECTOR lookAt = XMVectorAdd(eye, XMLoadFloat3(&this->params.forward_WS));
    const XMVECTOR up = XMLoadFloat3(&this->params.up_WS);
    const XMMATRIX worldToView = XMMatrixLookAtRH(eye, lookAt, up);
    XMStoreFloat4x4(&this->worldToViewMat, worldToView);

    const XMMATRIX viewToClip = XMMatrixPerspectiveFovRH(
        this->currentFovYRadians, this->aspectRatio, this->params.nearPlane, this->params.farPlane);
    XMStoreFloat4x4(&this->dlssMatrices.viewToClipMat, viewToClip);

    const XMMATRIX worldToClip = XMMatrixMultiply(worldToView, viewToClip);
    XMStoreFloat4x4(&this->params.worldToClipMat, worldToClip);

    XMVECTOR det;
    const XMMATRIX clipToView = XMMatrixInverse(&det, viewToClip);
    XMStoreFloat4x4(&this->dlssMatrices.clipToViewMat, clipToView);

    const XMMATRIX viewToWorld = XMMatrixInverse(&det, worldToView);
    XMStoreFloat4x4(&this->viewToWorldMat, viewToWorld);
    XMMATRIX worldToPrevView = XMLoadFloat4x4(&this->worldToPrevViewMat);
    if (instanceOffsetChanged)
    {
        // not sure if this correction is necessary for SL...
        const XMVECTOR instanceOffset = XMLoadSInt3(&this->params.instanceOffset);
        const XMVECTOR prevInstanceOffset = XMLoadSInt3(&this->params.prevInstanceOffset);
        const XMVECTOR translation = XMVectorSubtract(instanceOffset, prevInstanceOffset);
        const XMMATRIX translationMat = XMMatrixTranslationFromVector(translation);
        worldToPrevView = XMMatrixMultiply(translationMat, worldToPrevView);

        // ...but this one is necessary to fix motion vectors
        XMMATRIX worldToPrevClip = XMLoadFloat4x4(&this->params.worldToPrevClipMat);
        worldToPrevClip = XMMatrixMultiply(translationMat, worldToPrevClip);
        XMStoreFloat4x4(&this->params.worldToPrevClipMat, worldToPrevClip);
    }
    const XMMATRIX viewToPrevView = XMMatrixMultiply(viewToWorld, worldToPrevView);
    const XMMATRIX clipToPrevView = XMMatrixMultiply(clipToView, viewToPrevView);
    const XMMATRIX prevViewToPrevClip = XMLoadFloat4x4(&this->prevViewToPrevClipMat);
    const XMMATRIX clipToPrevClip = XMMatrixMultiply(clipToPrevView, prevViewToPrevClip);
    XMStoreFloat4x4(&this->dlssMatrices.clipToPrevClipMat, clipToPrevClip);
    const XMMATRIX prevClipToClip = XMMatrixInverse(&det, clipToPrevClip);
    XMStoreFloat4x4(&this->dlssMatrices.prevClipToClipMat, prevClipToClip);

    XMStoreFloat4x4(&this->worldToPrevViewMat, worldToView);
    this->prevViewToPrevClipMat = this->dlssMatrices.viewToClipMat;
}

static inline XMFLOAT3 toDirectXFloat3(const glm::vec3& v)
{
    return { v.x, v.y, v.z };
}

static inline XMINT3 toDirectXInt3(const glm::ivec3& v)
{
    return { v.x, v.y, v.z };
}

constexpr float mouseSensitivity = 0.0016f;

constexpr float fovTransitionSpeed = 10.f;
constexpr float zoomFovRatio = 0.3f;

void Camera::processInput(double deltaTime, const PlayerInput& input)
{
    this->params.worldToPrevClipMat = this->params.worldToClipMat; // if instanceOffset has changed, correction will be applied in setMatrices()
    this->params.prevJitter = this->params.jitter;
    this->params.prevPos_WS = this->params.pos_WS;
    this->params.prevForward_WS = this->params.forward_WS;
    this->params.prevRight_WS = this->params.right_WS;
    this->params.prevUp_WS = this->params.up_WS;
    this->params.prevTanHalfFovY = this->params.tanHalfFovY;

    if (input.linearInput.x != 0 || input.linearInput.y != 0 || input.linearInput.z != 0)
    {
        const float horizontalSpeed = SettingsManager::getAsFloat("movementSpeed");
        const float verticalSpeed = horizontalSpeed * 0.65f;

        const XMFLOAT3 linearSpeedBase(horizontalSpeed, verticalSpeed, horizontalSpeed);
        XMVECTOR linearSpeed = XMLoadFloat3(&linearSpeedBase);
        linearSpeed = XMVectorScale(linearSpeed, static_cast<float>(deltaTime) * input.linearSpeedMultiplier);
        const XMVECTOR linearMovement = XMVectorMultiply(linearSpeed, XMLoadFloat3(&input.linearInput));

        XMFLOAT3 storedLinearMovement;
        XMStoreFloat3(&storedLinearMovement, linearMovement);
        this->moveLinear(storedLinearMovement);

        this->areMatricesDirty = true;
    }

    if (input.mouseMovement.x != 0 || input.mouseMovement.y != 0)
    {
        this->rotate(input.mouseMovement.x * mouseSensitivity, input.mouseMovement.y * mouseSensitivity);
        this->areMatricesDirty = true;
    }

    const float targetFov = input.isZoomHeld ? this->defaultFovYRadians * zoomFovRatio : this->defaultFovYRadians;
    const float deltaFov = targetFov - this->currentFovYRadians;
    const float maxStep = fovTransitionSpeed * fabsf(deltaFov) * static_cast<float>(deltaTime);
    if (fabsf(deltaFov) > 0.0001f)
    {
        if (fabsf(deltaFov) <= maxStep)
        {
            this->currentFovYRadians = targetFov;
        }
        else
        {
            this->currentFovYRadians += (deltaFov > 0 ? maxStep : -maxStep);
        }

        this->params.tanHalfFovY = tanf(this->currentFovYRadians * 0.5f);
        this->areMatricesDirty = true;
    }

    for (int i = 0; i < 3; ++i)
    {
        float& floatPosComponent = this->posFloat_WS[i];
        if (floatPosComponent < 0.f || floatPosComponent > 1.f)
        {
            const int intPart = static_cast<int>(floor(floatPosComponent));
            this->posInt_WS[i] += intPart;
            floatPosComponent -= intPart;
        }
    }

    this->params.jitter = this->jitterHalton.next();
}

bool Camera::update()
{
    const Scene& scene = Renderer::getScene();

    const glm::vec3 paramsPos_WS = glm::vec3(this->getPosInt_WS() - scene.getInstanceOffset()) + this->getPosFloat_WS();
    this->params.pos_WS = toDirectXFloat3(paramsPos_WS);

    const glm::ivec3 instanceOffset = scene.getInstanceOffset();
    const glm::ivec3 prevInstanceOffset = scene.getPrevInstanceOffset();
    this->params.instanceOffset = toDirectXInt3(instanceOffset);
    this->params.prevInstanceOffset = toDirectXInt3(prevInstanceOffset);
    const bool instanceOffsetChanged = prevInstanceOffset != instanceOffset;

    const bool didChange = this->areMatricesDirty || instanceOffsetChanged;
    if (didChange)
    {
        this->setMatrices(instanceOffsetChanged);
        this->areMatricesDirty = false;
    }

    return didChange;
}

void Camera::setAspectRatio(float aspectRatio)
{
    this->aspectRatio = aspectRatio;
    this->areMatricesDirty = true;
}

inline sl::float3 toSlFloat3(const DirectX::XMFLOAT3& v)
{
    return { v.x, v.y, v.z };
}

inline sl::float4x4 toSlFloat4x4(const DirectX::XMFLOAT4X4& m)
{
    sl::float4x4 result;
    result.setRow(0, { m._11, m._12, m._13, m._14 });
    result.setRow(1, { m._21, m._22, m._23, m._24 });
    result.setRow(2, { m._31, m._32, m._33, m._34 });
    result.setRow(3, { m._41, m._42, m._43, m._44 });
    return result;
}

void Camera::copySlConstantsTo(sl::Constants* constants)
{
    constants->cameraViewToClip = toSlFloat4x4(this->dlssMatrices.viewToClipMat);
    constants->clipToCameraView = toSlFloat4x4(this->dlssMatrices.clipToViewMat);
    constants->clipToPrevClip = toSlFloat4x4(this->dlssMatrices.clipToPrevClipMat);
    constants->prevClipToClip = toSlFloat4x4(this->dlssMatrices.prevClipToClipMat);

    constants->jitterOffset = { 0.5f - this->params.jitter.x, 0.5f - this->params.jitter.y };
    constants->mvecScale = { 1, 1 };
    constants->cameraPinholeOffset = { 0, 0 };
    constants->cameraPos = toSlFloat3(this->params.pos_WS);
    constants->cameraUp = toSlFloat3(this->params.up_WS);
    constants->cameraRight = toSlFloat3(this->params.right_WS);
    constants->cameraFwd = toSlFloat3(this->params.forward_WS);

    constants->cameraNear = this->params.nearPlane;
    constants->cameraFar = this->params.farPlane;
    constants->cameraFOV = this->currentFovYRadians;
    constants->cameraAspectRatio = this->aspectRatio;
}

void Camera::copyMatricesToDlssOptions(sl::float4x4* worldToCameraView, sl::float4x4* cameraViewToWorld)
{
    *worldToCameraView = toSlFloat4x4(this->worldToViewMat);
    *cameraViewToWorld = toSlFloat4x4(this->viewToWorldMat);
}

void Camera::copyParamsTo(CameraParams* dest) const
{
    memcpy(dest, &this->params, sizeof(CameraParams));
}

void Camera::setPos_WS(glm::vec3 newPos)
{
    this->posInt_WS = glm::ivec3(0, 0, 0);
    this->posFloat_WS = newPos;
}

glm::vec3 Camera::getPos_WS() const
{
    return glm::vec3(this->posInt_WS) + this->posFloat_WS;
}

const glm::ivec3& Camera::getPosInt_WS() const
{
    return this->posInt_WS;
}

const glm::vec3& Camera::getPosFloat_WS() const
{
    return this->posFloat_WS;
}
