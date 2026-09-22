// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"
#include "common/common_params.h"

#include <vector>

class ToFreeList;

// Compacts the area light sampling structure on the GPU when instances leave the TLAS, so the
// main thread never rewrites the millions of entries a large world holds; see
// knowledge/scene/scene.md
namespace AreaLightCompactor
{

void init();

// Rewrites elements [firstElement, firstElement + numElements) of samplingStructure; ranges are
// relative to firstElement, in ascending newOffset order, and cover [0, numElements).
// samplingStructure is read in NON_PIXEL_SHADER_RESOURCE state and left there.
void dispatch(ID3D12GraphicsCommandList4* cmdList,
              ToFreeList& toFreeList,
              ID3D12Resource* samplingStructure,
              uint32_t firstElement,
              uint32_t numElements,
              const std::vector<AreaLightCompactRange>& ranges);

void destroy();

} // namespace AreaLightCompactor
