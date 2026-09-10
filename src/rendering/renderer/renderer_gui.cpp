// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <chrono>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <implot.h>

#include "rendering/camera.h"
#include "rendering/common/common_enums.h"
#include "rendering/window_manager.h"
#include "settings_gui_helpers.h"
#include "settings_manager.h"
#include "terrain/biome.h"
#include "terrain/terrain.h"

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
    imguiDX12InitInfo.RTVFormat = SWAP_CHAIN_FORMAT;

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

static uint32_t renderedFrameCount = 0;
static uint32_t presentedFrameCount = 0;
static double elapsedTime = 0.0;
static int lastFps = 0;
static int lastRenderedFps = 0;

void updateFps(double deltaTime)
{
    renderedFrameCount++;
    presentedFrameCount += renderState.frameGen.framesPresentedLastFrame;
    elapsedTime += deltaTime;

    if (elapsedTime >= 1.0)
    {
        lastFps = presentedFrameCount;
        lastRenderedFps = renderedFrameCount;
        renderedFrameCount = 0;
        presentedFrameCount = 0;
        elapsedTime = 0.0;
    }
}

static const std::vector<const char*> samplingModeComboOptions = {
    "naive",
    "MIS",
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
    "off", "pathTracing", "diffuseAlbedo", "specularAlbedo", "depth", "motion", "specularHitDistance", "normals", "debug",
};
static const std::vector<const char*> dlssModeOptions = {
    "DLAA", "quality", "balanced", "performance", "ultra performance",
};
static const std::vector<const char*> dlssPresetOptions = {
    "Preset D", "Preset F",
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
    bool radianceSettingsChanged = false;

    ImGui::SetNextWindowPos(ImVec2(10, 10));

    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("Settings", nullptr, windowFlags | ImGuiWindowFlags_AlwaysAutoResize))
    {
        radianceSettingsChanged |= SettingsGuiHelpers::InputUint("Max path depth", "maxPathDepth", 1, 16);
        SettingsGuiHelpers::ComboUint("Tonemapping", "tonemapping", tonemappingComboOptions);
        renderState.needsResize |= SettingsGuiHelpers::Checkbox("Enable path splitting", "doPathSplitting");

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("SHaRC");
        if (renderState.sharc.supported)
        {
            bool changed = SettingsGuiHelpers::Checkbox("Enable SHaRC", "sharc");
            bool viewChanged = false;
            if (ImGui::CollapsingHeader("SHaRC settings"))
            {
                changed |= SettingsGuiHelpers::SliderUint("Cache capacity log2", "sharcCapacityLog2", 16, 24);
                changed |= SettingsGuiHelpers::SliderUint("Update stride", "sharcDownscale", 1, 16);
                changed |= SettingsGuiHelpers::SliderFloat("Grid scale", "sharcSceneScale", 1.f, 200.f);
                changed |= SettingsGuiHelpers::SliderUint("History frames", "sharcAccumulationFrames", 1, 128);
                changed |= SettingsGuiHelpers::SliderUint("Stale frames", "sharcStaleFrames", 8, 256);
                viewChanged = SettingsGuiHelpers::ComboUint("SHaRC view", "sharcDebug", { "Beauty", "Cache hits", "Bounce count", "Hash grid", "Cached radiance" });
                if (ImGui::Button("Reset cache"))
                    changed = true;
            }
            renderState.sharc.resetRequested |= changed;
            renderState.didPathTracingSettingsChange |= changed || viewChanged;
        }
        else
            ImGui::TextUnformatted("SHaRC unavailable (native fp16 / int64 atomics required)");
        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Sampling");
        radianceSettingsChanged |= SettingsGuiHelpers::ComboUint("Sampling mode", "samplingMode", samplingModeComboOptions);

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Materials");
        radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Refraction indirect passthrough", "refractionIndirectPassthrough");

        if (renderState.voxelMode)
        {
            SettingsGuiHelpers::VerticalSpacing();
            SettingsGuiHelpers::SectionTitle("Fog");
            if (ImGui::CollapsingHeader("Fog settings"))
            {
                radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fog scattering", "fogScatteringMultiplier", 0.f, 10.f);
                radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fog scale height", "fogScaleHeight", 1.f, 200.f);
                radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fog anisotropy", "fogG", -0.99f, 0.99f);
                radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Fog march steps", "fogMarchSteps", 1, 16);
                radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fog ambient strength", "fogAmbientStrength", 0.f, 2.f);
            }
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
            renderState.needsResize |= SettingsGuiHelpers::ComboUint("DLSS preset", "dlssPreset", dlssPresetOptions);
            if (renderState.frameGen.supported)
            {
                SettingsGuiHelpers::Checkbox("Frame generation", "frameGeneration");
            }
            else
            {
                ImGui::TextDisabled("Frame generation not supported:\n- %s", renderState.frameGen.unsupportedReason.c_str());
            }
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
                radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Color chunks", "debugColorChunks");
            }

            SettingsGuiHelpers::VerticalSpacing();

            radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 0", "debugBool0");
            radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 1", "debugBool1");
            radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 2", "debugBool2");
            radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 3", "debugBool3");
            radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 0", "debugFloat0", -100.f, 100.f);
            radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 1", "debugFloat1", -100.f, 100.f);
            radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 2", "debugFloat2", -100.f, 100.f);
            radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 3", "debugFloat3", -100.f, 100.f);
        }
    }
    ImGui::End();
    renderState.sharc.resetRequested |= radianceSettingsChanged;
    renderState.didPathTracingSettingsChange |= radianceSettingsChanged;

    constexpr int performanceWindowHeight = 240;
    ImGui::SetNextWindowPos(ImVec2(10, renderState.viewport.Height - 10 - performanceWindowHeight));
    ImGui::SetNextWindowSize(ImVec2(800, performanceWindowHeight));

    if (ImGui::Begin("Performance", nullptr, windowFlags))
    {
        if (renderState.frameGen.active)
        {
            ImGui::Text("FPS: %d (%d rendered)", lastFps, lastRenderedFps);
        }
        else
        {
            ImGui::Text("FPS: %d", lastFps);
        }

        SettingsGuiHelpers::VerticalSpacing();
        renderState.frameTimeBuffer.push({ static_cast<float>(renderState.frameNumber), static_cast<float>(deltaTime) * 1000.f });
        if (ImPlot::BeginPlot("Frame time (rendered)", ImVec2(-1, -1)))
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

        Biome cameraBiome;
        ImGui::Text("Biome: %s",
                    Terrain::tryGetCameraBiome(cameraBiome) ? Biomes::getBiomeData(cameraBiome).name : "unknown");
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
