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

#define TERRAIN_TILE_SIZE_TEXELS 16.f

struct RayCone
{
    float width;
    float angle;
};

float computeMipLevel(const float coneWidth)
{
    return log2(max(coneWidth, 1e-6f) * TERRAIN_TILE_SIZE_TEXELS) + renderParams.mipBias;
}

float getRayConePixelAngle()
{
    return 2.f * atan(cameraParams.tanHalfFovY) / float(renderParams.renderSize.y);
}

float getRayConeWidthAtDistance(const RayCone rayCone, const float distanceTraveled)
{
    return rayCone.width + rayCone.angle * distanceTraveled;
}
