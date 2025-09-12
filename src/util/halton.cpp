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

#include "halton.h"

// https://en.wikipedia.org/wiki/Halton_sequence
void HaltonSequence::generateSequenceDimension(uint32_t dim, uint32_t base)
{
    int n = 0;
    int d = 1;
    for (int i = 0; i < this->sequence.size(); ++i)
    {
        int x = d - n;
        if (x == 1)
        {
            n = 1;
            d *= base;
        }
        else
        {
            int y = d / base;
            while (x <= y)
            {
                y /= base;
            }
            n = (base + 1) * y - x;
        }

        DirectX::XMFLOAT2& sequenceVal = this->sequence[i];
        (dim == 0 ? sequenceVal.x : sequenceVal.y) = n / static_cast<float>(d);
    }
}

void HaltonSequence::init(uint32_t sequenceLength)
{
    this->sequence.resize(sequenceLength);
    this->generateSequenceDimension(0, 2);
    this->generateSequenceDimension(1, 3);
    this->sequencePtr = 0;
}

DirectX::XMFLOAT2 HaltonSequence::next()
{
    DirectX::XMFLOAT2 nextVal = this->sequence[this->sequencePtr];
    if (++this->sequencePtr >= this->sequence.size())
    {
        this->sequencePtr = 0;
    }
    return nextVal;
}
