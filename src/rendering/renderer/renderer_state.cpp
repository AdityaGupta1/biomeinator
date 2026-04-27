// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

namespace Renderer
{

RendererState::RendererState()
{
    debugViewComboMap = {
        { "off", nullptr },

        { "pathTracing", &pathTracingTarget },
        { "diffuseAlbedo", &diffuseAlbedoTarget },
        { "specularAlbedo", &specularAlbedoTarget },
        { "linearDepth", &linearDepthTarget },
        { "motion", &motionTarget },
        { "specularHitDistance", &specularHitDistanceTarget },
        { "normals", &normalsAndRoughnessTarget },

        { "debug", &debugTarget },
    };
}

RendererState renderState;

DescriptorHeapAllocator sharedDescHeapAlloc;

} // namespace Renderer
