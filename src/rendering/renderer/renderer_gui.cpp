// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

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

static bool drawPathTracingTab()
{
    bool radianceSettingsChanged = false;

    SettingsGuiHelpers::SectionTitle("Path tracing");
    radianceSettingsChanged |= SettingsGuiHelpers::InputUint("Max path depth", "maxPathDepth", 1, 16);
    radianceSettingsChanged |= SettingsGuiHelpers::ComboUint("Sampling mode", "samplingMode", samplingModeComboOptions);
    renderState.needsResize |= SettingsGuiHelpers::Checkbox("Enable path splitting", "doPathSplitting");
    radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Refraction indirect passthrough", "refractionIndirectPassthrough");

    SettingsGuiHelpers::SectionTitle("SHaRC");
    if (renderState.sharc.supported)
    {
        bool changed = SettingsGuiHelpers::Checkbox("Enable SHaRC", "sharc");
        changed |= SettingsGuiHelpers::SliderUint("Cache capacity log2", "sharcCapacityLog2", 16, 24);
        changed |= SettingsGuiHelpers::SliderUint("Update stride", "sharcDownscale", 1, 16);
        changed |= SettingsGuiHelpers::SliderFloat("Grid scale", "sharcSceneScale", 1.f, 200.f);
        changed |= SettingsGuiHelpers::SliderUint("History frames", "sharcAccumulationFrames", 1, 128);
        changed |= SettingsGuiHelpers::SliderUint("Stale frames", "sharcStaleFrames", 8, 256);
        const bool viewChanged = SettingsGuiHelpers::ComboUint("SHaRC view", "sharcDebug", { "Beauty", "Cache hits", "Bounce count", "Hash grid", "Cached radiance" });
        if (ImGui::Button("Reset cache"))
        {
            changed = true;
        }
        renderState.sharc.resetRequested |= changed;
        renderState.didPathTracingSettingsChange |= changed || viewChanged;
    }
    else
    {
        ImGui::TextWrapped("SHaRC unavailable (native fp16 / int64 atomics required)");
    }

    return radianceSettingsChanged;
}

static void drawImageTab()
{
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
            ImGui::PushTextWrapPos();
            ImGui::TextDisabled("Frame generation not supported:\n- %s", renderState.frameGen.unsupportedReason.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    SettingsGuiHelpers::SectionTitle("Tonemapping");
    SettingsGuiHelpers::ComboUint("Tonemapping", "tonemapping", tonemappingComboOptions);
}

static bool drawAtmosphereTab()
{
    bool radianceSettingsChanged = false;

    SettingsGuiHelpers::SectionTitle("Sky");
    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Sky strength", "skyStrength", 0.f, 10.f);

    {
        const SettingsGuiHelpers::ScopedId id("clouds");

        SettingsGuiHelpers::SectionTitle("Clouds");
        radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Enable clouds", "clouds");
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Coverage", "cloudCoverage", 0.f, 1.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Base height", "cloudBaseHeight", 0.f, 10000.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Thickness", "cloudThickness", 10.f, 10000.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cell size", "cloudCellSize", 64.f, 4096.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Pattern scale", "cloudPatternScale", 256.f, 131072.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Pattern seed", "cloudSeed", 0, 65535);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind X", "cloudWindX", -50.f, 50.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind Z", "cloudWindZ", -50.f, 50.f);

        SettingsGuiHelpers::SectionTitle("Cloud lighting");
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Extinction", "cloudExtinction", 0.f, 0.1f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Lighting samples", "cloudSamples", 1, 32);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Ambient strength", "cloudAmbient", 0.f, 2.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Anisotropy", "cloudPhaseG", 0.f, 0.95f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Multiple scattering", "cloudMultiScatterStrength", 0.f, 2.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Draw distance", "cloudDrawDistance", 100.f, 100000.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Shadow distance", "cloudShadowDistance", 100.f, 20000.f);
    }

    {
        const SettingsGuiHelpers::ScopedId id("fog");

        SettingsGuiHelpers::SectionTitle("Fog");
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Scattering", "fogScatteringMultiplier", 0.f, 10.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Scale height", "fogScaleHeight", 1.f, 200.f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Anisotropy", "fogG", -0.99f, 0.99f);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("March steps", "fogMarchSteps", 1, 16);
        radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Ambient strength", "fogAmbientStrength", 0.f, 2.f);
    }

    return radianceSettingsChanged;
}

static void drawCameraTab()
{
    SettingsGuiHelpers::SectionTitle("Movement");
    SettingsGuiHelpers::SliderFloat("Movement speed", "movementSpeed", 1.f, 250.f);
}

static bool drawDebugTab()
{
    bool radianceSettingsChanged = false;

    SettingsGuiHelpers::SectionTitle("Debug view");
    SettingsGuiHelpers::ComboString("Debug view", "debugView", debugViewComboOptions);
    SettingsGuiHelpers::SliderFloat("Debug view scale", "debugViewScale", -1000.f, 1000.f);
    SettingsGuiHelpers::Checkbox("Debug view apply tonemap", "debugViewApplyTonemap");
    if (renderState.voxelMode)
    {
        radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Color chunks", "debugColorChunks");
    }

    SettingsGuiHelpers::SectionTitle("Debug values");
    radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 0", "debugBool0");
    radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 1", "debugBool1");
    radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 2", "debugBool2");
    radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Debug bool 3", "debugBool3");
    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 0", "debugFloat0", -100.f, 100.f);
    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 1", "debugFloat1", -100.f, 100.f);
    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 2", "debugFloat2", -100.f, 100.f);
    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Debug float 3", "debugFloat3", -100.f, 100.f);

    return radianceSettingsChanged;
}

void imguiEndFrame(double deltaTime)
{
    renderState.didPathTracingSettingsChange = false;
    bool radianceSettingsChanged = false;

    constexpr float windowMargin = 10.f;
    constexpr float settingsWindowWidth = 420.f;
    constexpr float performanceWindowHeight = 240.f;

    const float settingsWindowMaxHeight = renderState.viewport.Height - performanceWindowHeight - 3.f * windowMargin;
    ImGui::SetNextWindowPos(ImVec2(windowMargin, windowMargin));
    ImGui::SetNextWindowSizeConstraints(ImVec2(settingsWindowWidth, 0.f), ImVec2(settingsWindowWidth, settingsWindowMaxHeight));

    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("Settings", nullptr, windowFlags | ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::BeginTabBar("SettingsTabs"))
        {
            if (ImGui::BeginTabItem("Path tracing"))
            {
                radianceSettingsChanged |= drawPathTracingTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Image"))
            {
                drawImageTab();
                ImGui::EndTabItem();
            }
            if (renderState.voxelMode && ImGui::BeginTabItem("Atmosphere"))
            {
                radianceSettingsChanged |= drawAtmosphereTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Camera"))
            {
                drawCameraTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug"))
            {
                radianceSettingsChanged |= drawDebugTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    renderState.sharc.resetRequested |= radianceSettingsChanged;
    renderState.didPathTracingSettingsChange |= radianceSettingsChanged;

    ImGui::SetNextWindowPos(ImVec2(windowMargin, renderState.viewport.Height - windowMargin - performanceWindowHeight));
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

    constexpr float debugWindowWidth = 300.f;
    ImGui::SetNextWindowPos(ImVec2(renderState.viewport.Width - windowMargin - debugWindowWidth, windowMargin));
    ImGui::SetNextWindowSize(ImVec2(debugWindowWidth, -1));

    if (ImGui::Begin("Debug", nullptr, windowFlags))
    {
        const glm::vec3 cameraPos_WS = renderState.camera.getPos_WS();
        ImGui::Text("pos: (%.2f, %.2f, %.2f)", cameraPos_WS.x, cameraPos_WS.y, cameraPos_WS.z);

        const float cameraYawDegrees = glm::mod(glm::degrees(renderState.camera.getTheta()), 360.f);
        const float cameraPitchDegrees = glm::degrees(renderState.camera.getPhi());
        ImGui::Text("yaw: %.1f pitch: %.1f", cameraYawDegrees, cameraPitchDegrees);

        if (renderState.voxelMode)
        {
            ImGui::Text("seed: %u", SettingsManager::getWorldSeed());

            Biome cameraBiome;
            ImGui::Text("biome: %s",
                        Terrain::tryGetCameraBiome(cameraBiome) ? Biomes::getBiomeData(cameraBiome).name : "unknown");
        }
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
