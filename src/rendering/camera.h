// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "rendering/dxr_includes.h"
#include "rendering/host_structs.h"
#include "rendering/common/common_params.h"
#include "rendering/common/common_structs.h"
#include "util/halton.h"

#include <numbers>

#include <glm/glm.hpp>
#include <sl_consts.h>

class Camera
{
private:
    CameraParams params{};

    glm::ivec3 posInt_WS{};
    glm::vec3 posFloat_WS{};

    bool areMatricesDirty{ true };
    struct
    {
        DirectX::XMFLOAT4X4 viewToClipMat;
        DirectX::XMFLOAT4X4 clipToViewMat;

        DirectX::XMFLOAT4X4 clipToPrevClipMat;
        DirectX::XMFLOAT4X4 prevClipToClipMat;
    } dlssMatrices;

    DirectX::XMFLOAT4X4 worldToPrevViewMat;
    DirectX::XMFLOAT4X4 prevViewToPrevClipMat;

    DirectX::XMFLOAT4X4 worldToViewMat;
    DirectX::XMFLOAT4X4 viewToWorldMat;

    HaltonSequence jitterHalton;

    float phi{ 0 };
    float theta{ std::numbers::pi_v<float> };

    float defaultFovYRadians{ 0 };
    float currentFovYRadians{ 0 };
    float aspectRatio{ 1.f };

    void setDirectionVectorsFromAngles();

    void moveLinear(DirectX::XMFLOAT3 linearMovement);
    void rotate(float dTheta, float dPhi);

    void setMatrices(bool globalInstanceOffsetChanged);

    void setPos_WS(glm::vec3 newPos);

public:
    void init(float defaultFovYRadians);

    void setJitterHaltonSequenceLength(uint32_t sequenceLength);

    void processInput(double deltaTime, const PlayerInput& input);
    // Deterministic per-frame motion for tests: camera-relative translation and a yaw step
    void applyScriptedMotion(DirectX::XMFLOAT3 linearMovement, float dTheta);
    bool update();
    void setAspectRatio(float aspectRatio);

    void copySlConstantsTo(sl::Constants* constants);
    void copyMatricesToDlssOptions(sl::float4x4* worldToCameraView, sl::float4x4* cameraViewToWorld);
    void copyParamsTo(CameraParams* dest) const;

    glm::vec3 getPos_WS() const;
    const glm::ivec3& getPosInt_WS() const;
    const glm::vec3& getPosFloat_WS() const;

    float getPhi() const;
    float getTheta() const;
    void restoreFromImport(glm::ivec3 posInt, glm::vec3 posFloat, float phi, float theta);
};
