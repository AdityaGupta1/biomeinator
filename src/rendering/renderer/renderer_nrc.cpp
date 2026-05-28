// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "NrcD3d12.h"
#undef min
#undef max

#include "scene/scene.h"
#include "terrain/chunk.h"
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
        // Hit positions reach NRC in render space (globalInstanceOffset already subtracted by the
        // TLAS), so derive a static camera-centric AABB rather than re-Configure on every chunk move.
        // The actual render-space AABB is camera-centric and bounded by ±(renderDistance+1)*chunkSizeXZ
        // in XZ; pad to absorb the sub-chunk remainder and any glTF/area-light geometry just outside.
        const int renderDistance = SettingsManager::getAsInt("renderDistance");
        const int paddingXZ = 32;
        const int halfExtentXZ = (renderDistance + 1) * static_cast<int>(chunkSizeXZ) + paddingXZ;
        cs.sceneBoundsMin = { static_cast<float>(-halfExtentXZ), 0.f, static_cast<float>(-halfExtentXZ) };
        cs.sceneBoundsMax = { static_cast<float>( halfExtentXZ), static_cast<float>(chunkSizeY), static_cast<float>( halfExtentXZ) };
    }
    else
    {
        if (renderState.scene.hasBounds())
        {
            const glm::ivec3& offset = renderState.scene.getGlobalInstanceOffset();
            const glm::vec3 offsetF = { static_cast<float>(offset.x), static_cast<float>(offset.y), static_cast<float>(offset.z) };
            const glm::vec3 boundsMin = renderState.scene.getBoundsMin_WS() - offsetF;
            const glm::vec3 boundsMax = renderState.scene.getBoundsMax_WS() - offsetF;
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
