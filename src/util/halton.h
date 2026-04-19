// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

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
