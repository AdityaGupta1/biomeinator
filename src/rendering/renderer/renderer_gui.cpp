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
            SettingsGuiHelpers::SectionTitle("Atmosphere");
            radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Sky strength (excluding sun)", "skyStrength", 0.f, 10.f, "%.2fx");
            if (ImGui::CollapsingHeader("Cloud settings"))
            {
                radianceSettingsChanged |= SettingsGuiHelpers::Checkbox("Clouds", "clouds");
                float coveragePercent = SettingsManager::getAsFloat("cloudCoverage") * 100.f;
                if (ImGui::SliderFloat("Cloud coverage", &coveragePercent, 0.f, 100.f, "%.0f%%"))
                {
                    SettingsManager::setAsFloat("cloudCoverage", std::clamp(coveragePercent, 0.f, 100.f) * 0.01f);
                    radianceSettingsChanged = true;
                }
                radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cloud density", "cloudDensity", 0.f, 0.04f);
                radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Cloud march steps", "cloudSteps", 8, 128);
                ImGui::TextDisabled("Ctrl+click sliders to enter exact values.");
                ImGui::TextDisabled("Higher frequencies make smaller features.");


                if (ImGui::TreeNode("Layer"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cloud base (m)", "cloudBaseHeight", 200.0f, 3000.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Layer thickness (m)", "cloudThickness", 100.0f, 1500.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cloud tile size (m)", "cloudPeriod", 1024.0f, 65536.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cloud draw distance (m)", "cloudMaxDistance", 4000.0f, 100000.0f);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Cloud shape"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion noise scale", "cloudWarpScale", 0.5f, 8.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion noise detail", "cloudWarpDetail", 0.0f, 4.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion noise roughness", "cloudWarpRoughness", 0.0f, 1.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion strength", "cloudWarpStrength", 0.0f, 2.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Voronoi Smooth F1 smoothness", "cloudVoronoiSmoothness", 0.0f, 1.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Voronoi randomness", "cloudVoronoiRandomness", 0.0f, 1.0f, "%.3f");
                    ImGui::TextWrapped("Smooth F1 blends adjacent cells. Noise scales snap to 1/16 to keep the cloud tile seamless.");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Surface detail"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail noise scale", "cloudFineScale", 1.0f, 64.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail noise octaves", "cloudFineDetail", 0.0f, 4.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail noise roughness", "cloudFineRoughness", 0.0f, 1.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail noise strength", "cloudFineStrength", 0.0f, 0.5f, "%.3f");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Height and density"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Height ramp white position", "cloudHeightRampEnd", 0.01f, 1.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Height influence", "cloudHeightGain", 0.0f, 12.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Density ramp white position", "cloudDensityRampStart", 0.0f, 0.8f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Density ramp black position", "cloudDensityRampEnd", 0.001f, 1.0f, "%.3f");
                    ImGui::TextWrapped("The layer base is a hard cut, matching the Blender reference. Keep the black ramp position above the white position.");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Lighting"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Sky ambient strength", "cloudAmbient", 0.0f, 2.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Forward scattering", "cloudPhaseG", 0.0f, 0.95f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Multiple scattering", "cloudMultiScatter", 0.0f, 3.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Powder effect", "cloudPowder", 0.0f, 2.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distance haze", "cloudAerial", 0.0f, 0.0001f, "%.6f");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Evolution"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion time offset", "cloudWarpTime", -1000.0f, 1000.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Distortion evolution speed", "cloudWarpSpeed", 0.0f, 0.2f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cell time offset", "cloudVoronoiTime", -1000.0f, 1000.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cell evolution speed", "cloudVoronoiSpeed", 0.0f, 0.2f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail time offset", "cloudFineTime", -1000.0f, 1000.0f, "%.3f");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Detail evolution speed", "cloudFineSpeed", 0.0f, 0.5f, "%.3f");
                    ImGui::TextWrapped("Each noise uses time offset + animation time * speed. Zero speed freezes evolution; pausing animation freezes evolution and wind.");
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Wind"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind X (m/s)", "cloudWindX", -50.0f, 50.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind Z (m/s)", "cloudWindZ", -50.0f, 50.0f);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Quality"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Sun ray march steps", "cloudLightSteps", 4, 32);
                    ImGui::TextWrapped("Density samples along one sun ray, not additional sun rays.");
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Broad ray samples", "cloudSecondarySteps", 4, 32);
                    const bool resolutionChanged = SettingsGuiHelpers::SliderUint("Primary resolution divisor", "cloudViewDownscale", 1, 4);
                    renderState.needsResize |= resolutionChanged;
                    radianceSettingsChanged |= resolutionChanged;
                    ImGui::TreePop();
                }
                if (ImGui::Button("Reset cloud defaults"))
                {
                    SettingsManager::setAsBool("clouds", true);
                    SettingsManager::setAsFloat("cloudCoverage", 0.5f);
                    SettingsManager::setAsFloat("cloudDensity", 0.007f);
                    SettingsManager::setAsUint("cloudSteps", 128u);
                    SettingsManager::setAsFloat("cloudBaseHeight", 1500.0f);
                    SettingsManager::setAsFloat("cloudThickness", 1500.0f);
                    SettingsManager::setAsFloat("cloudPeriod", 55368.347656f);
                    SettingsManager::setAsFloat("cloudMaxDistance", 100000.0f);
                    SettingsManager::setAsFloat("cloudWarpScale", 2.5f);
                    SettingsManager::setAsFloat("cloudWarpDetail", 2.0f);
                    SettingsManager::setAsFloat("cloudWarpRoughness", 0.5f);
                    SettingsManager::setAsFloat("cloudWarpStrength", 1.0f);
                    SettingsManager::setAsFloat("cloudVoronoiSmoothness", 1.0f);
                    SettingsManager::setAsFloat("cloudVoronoiRandomness", 0.666667f);
                    SettingsManager::setAsFloat("cloudFineScale", 30.0f);
                    SettingsManager::setAsFloat("cloudFineDetail", 2.0f);
                    SettingsManager::setAsFloat("cloudFineRoughness", 0.5f);
                    SettingsManager::setAsFloat("cloudFineStrength", 0.1f);
                    SettingsManager::setAsFloat("cloudHeightRampEnd", 0.177273f);
                    SettingsManager::setAsFloat("cloudHeightGain", 6.1f);
                    SettingsManager::setAsFloat("cloudDensityRampStart", 0.145455f);
                    SettingsManager::setAsFloat("cloudDensityRampEnd", 0.363636f);
                    SettingsManager::setAsFloat("cloudAmbient", 0.68f);
                    SettingsManager::setAsFloat("cloudPhaseG", 0.65f);
                    SettingsManager::setAsFloat("cloudMultiScatter", 1.0f);
                    SettingsManager::setAsFloat("cloudPowder", 1.0f);
                    SettingsManager::setAsFloat("cloudAerial", 2.5e-05f);
                    SettingsManager::setAsFloat("cloudWindX", 8.0f);
                    SettingsManager::setAsFloat("cloudWindZ", 3.0f);
                    SettingsManager::setAsUint("cloudLightSteps", 12u);
                    SettingsManager::setAsUint("cloudSecondarySteps", 8u);
                    SettingsManager::setAsUint("cloudViewDownscale", 2u);
                    SettingsManager::setAsFloat("cloudWarpTime", 0.0f);
                    SettingsManager::setAsFloat("cloudWarpSpeed", 0.02f);
                    SettingsManager::setAsFloat("cloudVoronoiTime", 0.0f);
                    SettingsManager::setAsFloat("cloudVoronoiSpeed", 0.01f);
                    SettingsManager::setAsFloat("cloudFineTime", 0.0f);
                    SettingsManager::setAsFloat("cloudFineSpeed", 0.05f);
                    renderState.needsResize = true;
                    radianceSettingsChanged = true;
                }
                if (ImGui::Button("Copy cloud settings"))
                {
                    std::string text = std::string("--clouds=") + (SettingsManager::getAsBool("clouds") ? "true" : "false") + " --cloudCoverage=" + std::to_string(SettingsManager::getAsFloat("cloudCoverage")) +
                        " --cloudDensity=" + std::to_string(SettingsManager::getAsFloat("cloudDensity")) +
                        " --cloudSteps=" + std::to_string(SettingsManager::getAsUint("cloudSteps"));
                    text += " --cloudBaseHeight=" + std::to_string(SettingsManager::getAsFloat("cloudBaseHeight"));
                    text += " --cloudThickness=" + std::to_string(SettingsManager::getAsFloat("cloudThickness"));
                    text += " --cloudPeriod=" + std::to_string(SettingsManager::getAsFloat("cloudPeriod"));
                    text += " --cloudMaxDistance=" + std::to_string(SettingsManager::getAsFloat("cloudMaxDistance"));
                    text += " --cloudWarpScale=" + std::to_string(SettingsManager::getAsFloat("cloudWarpScale"));
                    text += " --cloudWarpDetail=" + std::to_string(SettingsManager::getAsFloat("cloudWarpDetail"));
                    text += " --cloudWarpRoughness=" + std::to_string(SettingsManager::getAsFloat("cloudWarpRoughness"));
                    text += " --cloudWarpStrength=" + std::to_string(SettingsManager::getAsFloat("cloudWarpStrength"));
                    text += " --cloudVoronoiSmoothness=" + std::to_string(SettingsManager::getAsFloat("cloudVoronoiSmoothness"));
                    text += " --cloudVoronoiRandomness=" + std::to_string(SettingsManager::getAsFloat("cloudVoronoiRandomness"));
                    text += " --cloudFineScale=" + std::to_string(SettingsManager::getAsFloat("cloudFineScale"));
                    text += " --cloudFineDetail=" + std::to_string(SettingsManager::getAsFloat("cloudFineDetail"));
                    text += " --cloudFineRoughness=" + std::to_string(SettingsManager::getAsFloat("cloudFineRoughness"));
                    text += " --cloudFineStrength=" + std::to_string(SettingsManager::getAsFloat("cloudFineStrength"));
                    text += " --cloudHeightRampEnd=" + std::to_string(SettingsManager::getAsFloat("cloudHeightRampEnd"));
                    text += " --cloudHeightGain=" + std::to_string(SettingsManager::getAsFloat("cloudHeightGain"));
                    text += " --cloudDensityRampStart=" + std::to_string(SettingsManager::getAsFloat("cloudDensityRampStart"));
                    text += " --cloudDensityRampEnd=" + std::to_string(SettingsManager::getAsFloat("cloudDensityRampEnd"));
                    text += " --cloudAmbient=" + std::to_string(SettingsManager::getAsFloat("cloudAmbient"));
                    text += " --cloudPhaseG=" + std::to_string(SettingsManager::getAsFloat("cloudPhaseG"));
                    text += " --cloudMultiScatter=" + std::to_string(SettingsManager::getAsFloat("cloudMultiScatter"));
                    text += " --cloudPowder=" + std::to_string(SettingsManager::getAsFloat("cloudPowder"));
                    text += " --cloudAerial=" + std::to_string(SettingsManager::getAsFloat("cloudAerial"));
                    text += " --cloudWindX=" + std::to_string(SettingsManager::getAsFloat("cloudWindX"));
                    text += " --cloudWindZ=" + std::to_string(SettingsManager::getAsFloat("cloudWindZ"));
                    text += " --cloudLightSteps=" + std::to_string(SettingsManager::getAsUint("cloudLightSteps"));
                    text += " --cloudSecondarySteps=" + std::to_string(SettingsManager::getAsUint("cloudSecondarySteps"));
                    text += " --cloudViewDownscale=" + std::to_string(SettingsManager::getAsUint("cloudViewDownscale"));
                    text += " --cloudWarpTime=" + std::to_string(SettingsManager::getAsFloat("cloudWarpTime"));
                    text += " --cloudWarpSpeed=" + std::to_string(SettingsManager::getAsFloat("cloudWarpSpeed"));
                    text += " --cloudVoronoiTime=" + std::to_string(SettingsManager::getAsFloat("cloudVoronoiTime"));
                    text += " --cloudVoronoiSpeed=" + std::to_string(SettingsManager::getAsFloat("cloudVoronoiSpeed"));
                    text += " --cloudFineTime=" + std::to_string(SettingsManager::getAsFloat("cloudFineTime"));
                    text += " --cloudFineSpeed=" + std::to_string(SettingsManager::getAsFloat("cloudFineSpeed"));
                    ImGui::SetClipboardText(text.c_str());
                }
            }
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
