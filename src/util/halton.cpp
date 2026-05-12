// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
    // Length 0 means "no jitter": emit zeros every frame.
    if (sequenceLength == 0)
    {
        this->sequence.assign(1, DirectX::XMFLOAT2{ 0.f, 0.f });
        this->sequencePtr = 0;
        return;
    }

    this->sequence.resize(sequenceLength);
    this->generateSequenceDimension(0, 2);
    this->generateSequenceDimension(1, 3);
    this->sequencePtr = 0;
}

DirectX::XMFLOAT2 HaltonSequence::next()
{
    const DirectX::XMFLOAT2 nextVal = this->sequence[this->sequencePtr];
    if (++this->sequencePtr >= this->sequence.size())
    {
        this->sequencePtr = 0;
    }
    return nextVal;
}
