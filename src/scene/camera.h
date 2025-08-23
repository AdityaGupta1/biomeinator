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
#include "rendering/common/common_structs.h"

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
