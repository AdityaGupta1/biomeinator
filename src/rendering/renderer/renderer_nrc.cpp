// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "NrcD3d12.h"
#undef min
#undef max

#include "scene/scene.h"
#include "terrain/terrain.h"
#include "settings_manager.h"
#include "logger.h"

namespace Renderer
{

void configureNrc()
{
    nrc::ContextSettings cs;
    const bool doPathSplitting = SettingsManager::getAsBool("doPathSplitting");
    cs.frameDimensions = { renderState.renderWidth * (doPathSplitting ? 2u : 1u), renderState.renderHeight };
    cs.trainingDimensions = nrc::ComputeIdealTrainingDimensions(cs.frameDimensions, 0);
    cs.maxPathVertices = SettingsManager::getAsUint("maxPathDepth");
    cs.samplesPerPixel = 1;
    cs.includeDirectLighting = false;
    cs.learnIrradiance = false;
    if (renderState.voxelMode)
    {
        const glm::ivec3 boundsMin = Terrain::getVoxelRenderBoundsMin_WS();
        const glm::ivec3 boundsMax = Terrain::getVoxelRenderBoundsMax_WS();
        cs.sceneBoundsMin = { static_cast<float>(boundsMin.x), static_cast<float>(boundsMin.y), static_cast<float>(boundsMin.z) };
        cs.sceneBoundsMax = { static_cast<float>(boundsMax.x), static_cast<float>(boundsMax.y), static_cast<float>(boundsMax.z) };
    }
    else
    {
        if (renderState.scene.hasBounds())
        {
            const glm::vec3& boundsMin = renderState.scene.getBoundsMin_WS();
            const glm::vec3& boundsMax = renderState.scene.getBoundsMax_WS();
            cs.sceneBoundsMin = { boundsMin.x, boundsMin.y, boundsMin.z };
            cs.sceneBoundsMax = { boundsMax.x, boundsMax.y, boundsMax.z };
        }
        else
        {
            Logger::logWarning("Scene has no bounds, using fallback");
            cs.sceneBoundsMin = { -10000.f, -10000.f, -10000.f };
            cs.sceneBoundsMax = {  10000.f,  10000.f,  10000.f };
        }
    }
    Logger::log("NRC scene bounds: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)",
                cs.sceneBoundsMin.x,
                cs.sceneBoundsMin.y,
                cs.sceneBoundsMin.z,
                cs.sceneBoundsMax.x,
                cs.sceneBoundsMax.y,
                cs.sceneBoundsMax.z);
    renderState.nrcContext->Configure(cs);
}

void initNrc()
{
    if (renderState.nrcContext != nullptr)
    {
        return;
    }

    nrc::GlobalSettings globalSettings;
    globalSettings.enableGPUMemoryAllocation = true;
    globalSettings.enableDebugBuffers = true;
    globalSettings.maxNumFramesInFlight = NUM_FRAMES_IN_FLIGHT;
    nrc::d3d12::Initialize(globalSettings);

    nrc::d3d12::Context::Create(renderState.device.Get(), renderState.nrcContext);
    configureNrc();
}

void destroyNrc()
{
    if (renderState.nrcContext == nullptr)
    {
        return;
    }
    flush();
    nrc::d3d12::Context::Destroy(*renderState.nrcContext);
    renderState.nrcContext = nullptr;
    nrc::d3d12::Shutdown();
}

} // namespace Renderer
