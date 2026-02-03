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
    bool update();
    void setAspectRatio(float aspectRatio);

    void copySlConstantsTo(sl::Constants* constants);
    void copyMatricesToDlssOptions(sl::float4x4* worldToCameraView, sl::float4x4* cameraViewToWorld);
    void copyParamsTo(CameraParams* dest) const;

    glm::vec3 getPos_WS() const;
    const glm::ivec3& getPosInt_WS() const;
    const glm::vec3& getPosFloat_WS() const;
};
