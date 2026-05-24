// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <chrono>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <implot.h>

#include "NrcD3d12.h"

#include "rendering/camera.h"
#include "rendering/window_manager.h"
#include "rendering/common/common_enums.h"
#include "settings_manager.h"
#include "settings_gui_helpers.h"

using WindowManager::hwnd;

namespace Renderer
{

void initImgui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = NULL;
    io.LogFilename = NULL;

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo imguiDX12InitInfo = {};
    imguiDX12InitInfo.Device = renderState.device.Get();
    imguiDX12InitInfo.CommandQueue = renderState.graphicsCmdQueue.Get();
    imguiDX12InitInfo.NumFramesInFlight = NUM_FRAMES_IN_FLIGHT;
    imguiDX12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    imguiDX12InitInfo.SrvDescriptorHeap = renderState.sharedDescriptorHeap.Get();
    imguiDX12InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                            D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    { sharedDescHeapAlloc.alloc(outCpuHandle, outGpuHandle); };
    imguiDX12InitInfo.SrvDescriptorFreeFn =
        [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
    { sharedDescHeapAlloc.free(cpuHandle, gpuHandle); };

    ImGui_ImplDX12_Init(&imguiDX12InitInfo);
}

static uint32_t frameCount = 0;
static double elapsedTime = 0.0;
static int lastFps = 0;

void updateFps(double deltaTime)
{
    frameCount++;
    elapsedTime += deltaTime;

    if (elapsedTime >= 1.0)
    {
        lastFps = frameCount;
        frameCount = 0;
        elapsedTime = 0.0;
    }
}

static const std::vector<const char*> samplingModeComboOptions = {
    "naive",
    "MIS",
    "RIS",
    "RTSL",
};
static const std::vector<const char*> antialiasingModeComboOptions = {
    "none",
    "accumulate",
    "DLSS",
};
static const std::vector<const char*> tonemappingComboOptions = {
    "none",
    "standard",
    "AgX",
    "Khronos PBR neutral",
};
static const std::vector<const char*> debugViewComboOptions = {
    "off", "pathTracing", "diffuseAlbedo", "specularAlbedo", "linearDepth", "motion", "specularHitDistance", "normals", "debug",
};
static const std::vector<const char*> dlssModeOptions = {
    "DLAA", "quality", "balanced", "performance", "ultra performance",
};

void imguiBeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void imguiEndFrame(double deltaTime)
{
    renderState.didPathTracingSettingsChange = false;

    ImGui::SetNextWindowPos(ImVec2(10, 10));

    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("Settings", nullptr, windowFlags | ImGuiWindowFlags_AlwaysAutoResize))
    {
        renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::InputUint("Max path depth", "maxPathDepth", 1, 16);
        SettingsGuiHelpers::ComboUint("Tonemapping", "tonemapping", tonemappingComboOptions);
        renderState.needsResize |= SettingsGuiHelpers::Checkbox("Enable path splitting", "doPathSplitting");

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Sampling");
        renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::ComboUint("Sampling mode", "samplingMode", samplingModeComboOptions);

        if (ImGui::CollapsingHeader("RTSL Cache", ImGuiTreeNodeFlags_DefaultOpen))
        {
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Enable RTSL cache", "rtslCacheEnabled");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderInt("Levels (L)", "rtslCacheLevels", 3, 7);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Uniform fraction", "rtslCacheUniformFrac", 0.05f, 1.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderInt("Lights per cell", "rtslCacheLightsPerCell", 1, RTSL_LIGHT_CACHE_K_MAX);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Reject depth (rel)", "rtslCacheRejectDepthRel", 0.f, 1.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Reject normal (cos)", "rtslCacheRejectNormalCos", 0.f, 1.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Depth bucket scale", "rtslCacheDepthBucketScale", 0.01f, 4.f);
            // Stat decay and eviction prior change only the counter arithmetic, not
            // cell addressing/acceptance, so they deliberately do NOT wire
            // didPathTracingSettingsChange: a suppressPrev reset would empty every
            // cell and force a ~20-frame re-warm on each slider nudge, making the
            // steady-state behaviour these knobs tune impossible to observe.
            SettingsGuiHelpers::SliderFloat("Stat decay", "rtslCacheStatDecay", 0.5f, 0.99f);
            SettingsGuiHelpers::SliderFloat("Evict prior strength", "rtslCacheEvictPriorStrength", 0.f, 16.f);
        }

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Materials");
        renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Refraction indirect passthrough", "refractionIndirectPassthrough");

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Radiance Cache");
        const bool didNrcChange = SettingsGuiHelpers::Checkbox("Enable NRC", "nrcEnabled");
        renderState.didPathTracingSettingsChange |= didNrcChange;
        renderState.needsResize |= didNrcChange;

        if (ImGui::CollapsingHeader("NRC Settings"))
        {
            {
                int nrcResolveModeInt = static_cast<int>(SettingsManager::getAsUint("nrcResolveMode"));
                SettingsGuiHelpers::ScopedItemWidth width(SettingsGuiHelpers::comboWidth);
                if (ImGui::Combo("NRC resolve mode", &nrcResolveModeInt, nrc::GetImGuiResolveModeComboString()))
                {
                    SettingsManager::setAsUint("nrcResolveMode", static_cast<uint32_t>(nrcResolveModeInt));
                    renderState.didPathTracingSettingsChange = true;
                }
            }

            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Max radiance", "nrcMaxRadiance", 0.01f, 100.0f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Termination threshold", "nrcTerminationThreshold", 0.001f, 1.0f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Training termination threshold", "nrcTrainingTerminationThreshold", 0.001f, 1.0f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Skip delta vertices", "nrcSkipDeltaVertices");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Train the cache", "nrcTrainTheCache");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Learning rate", "nrcLearningRate", 0.0001f, 0.1f);
        }

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Antialiasing");
        const bool didAntialiasingChange = SettingsGuiHelpers::ComboUint("Antialiasing mode", "antialiasingMode", antialiasingModeComboOptions);
        renderState.needsResize |= didAntialiasingChange; // technically should need resize only when switching to or from DLSS, but whatever
        renderState.didPathTracingSettingsChange |= didAntialiasingChange;
        const AntialiasingMode antialiasingMode = static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));

        if (antialiasingMode == AntialiasingMode::ACCUMULATE)
        {
            ImGui::Text("accumulated frames: %u", renderState.accumulatedFrameNumber);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderUint("Max accumulated frames", "maxAccumulatedFrames", 1, 2048);
        }
        else if (antialiasingMode == AntialiasingMode::DLSS)
        {
            renderState.needsResize |= SettingsGuiHelpers::ComboUint("DLSS mode", "dlssMode", dlssModeOptions);
        }

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("World");
        SettingsGuiHelpers::SliderFloat("Movement speed", "movementSpeed", 1.f, 250.f);

        SettingsGuiHelpers::VerticalSpacing();

        if (ImGui::CollapsingHeader("Debug", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SettingsGuiHelpers::SectionTitle("Debug view");
            SettingsGuiHelpers::ComboString("Debug view", "debugView", debugViewComboOptions);
            SettingsGuiHelpers::SliderFloat("Debug view scale", "debugViewScale", -1000.f, 1000.f);
            SettingsGuiHelpers::Checkbox("Debug view apply tonemap", "debugViewApplyTonemap");

            SettingsGuiHelpers::VerticalSpacing();

            if (renderState.voxelMode)
            {
                renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Color chunks", "debugColorChunks");
            }

            SettingsGuiHelpers::VerticalSpacing();

            // TODO(cleanup): temporary RTSL-cache debug label; revert to "Debug bool 0"
            // when the NEE-only instrumentation in path_tracing.rgs.hlsl is removed.
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("NEE only", "debugBool0");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 1", "debugBool1");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 2", "debugBool2");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 3", "debugBool3");
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 0", "debugFloat0", -100.f, 100.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 1", "debugFloat1", -100.f, 100.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 2", "debugFloat2", -100.f, 100.f);
            renderState.didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 3", "debugFloat3", -100.f, 100.f);
        }
    }
    ImGui::End();

    constexpr int performanceWindowHeight = 240;
    ImGui::SetNextWindowPos(ImVec2(10, renderState.viewport.Height - 10 - performanceWindowHeight));
    ImGui::SetNextWindowSize(ImVec2(800, performanceWindowHeight));

    if (ImGui::Begin("Performance", nullptr, windowFlags))
    {
        ImGui::Text("FPS: %d", lastFps);

        SettingsGuiHelpers::VerticalSpacing();
        renderState.frameTimeBuffer.push({ static_cast<float>(renderState.frameNumber), static_cast<float>(deltaTime) * 1000.f });
        if (ImPlot::BeginPlot("Frame time", ImVec2(-1, -1)))
        {
            static constexpr ImPlotAxisFlags axisFlags = 0;
            ImPlot::SetupAxes(nullptr, nullptr, axisFlags, axisFlags);
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    static_cast<int>(renderState.frameNumber) - static_cast<int>(renderState.frameTimeBuffer.getMaxSize()),
                                    renderState.frameNumber,
                                    ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 20);
            ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);
            ImPlot::PlotShaded("Frame time",
                               &renderState.frameTimeBuffer.getData()[0].frameIdx,
                               &renderState.frameTimeBuffer.getData()[0].timeMs,
                               static_cast<int>(renderState.frameTimeBuffer.getSize()),
                               -INFINITY,
                               ImPlotItemFlags_NoLegend,
                               renderState.frameTimeBuffer.getOffset(),
                               sizeof(FrameTimeMeasurement));
            ImPlot::EndPlot();
        }
    }
    ImGui::End();

    constexpr int debugWindowWidth = 300;
    ImGui::SetNextWindowPos(ImVec2(renderState.viewport.Width - 10 - debugWindowWidth, 10));
    ImGui::SetNextWindowSize(ImVec2(debugWindowWidth, -1));

    if (ImGui::Begin("Debug", nullptr, windowFlags))
    {
        const glm::vec3 cameraPos_WS = renderState.camera.getPos_WS();
        ImGui::Text("Position: (%.2f, %.2f, %.2f)", cameraPos_WS.x, cameraPos_WS.y, cameraPos_WS.z);
    }
    ImGui::End();

    if (renderState.frameNumber == 0)
    {
        ImGui::SetWindowFocus(NULL);
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), renderState.cmdList.Get());
}

} // namespace Renderer
