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

#include <cstdint>
#include <DirectXMath.h>
#include <vector>

class HaltonSequence
{
private:
    std::vector<DirectX::XMFLOAT2> sequence;
    uint32_t sequencePtr;

    void generateSequenceDimension(uint32_t dim, uint32_t base);

public:
    void init(uint32_t sequenceLength);

    DirectX::XMFLOAT2 next();
};
