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
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Weather tile size (m)", "cloudPeriod", 1024.0f, 16384.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Cloud draw distance (m)", "cloudMaxDistance", 4000.0f, 100000.0f);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Shape"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Large shape frequency", "cloudWeatherScale", 1, 4);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Billow frequency", "cloudBillowScale", 1, 8);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Perlin / cellular blend", "cloudPerlinWeight", 0.0f, 1.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Billow threshold", "cloudShapeThreshold", 0.0f, 0.6f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Billow contrast", "cloudShapeGain", 0.5f, 8.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Weather edge softness", "cloudWeatherSoftness", 0.005f, 0.15f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Bottom softness", "cloudBottomFade", 0.02f, 0.4f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Top fade start", "cloudTopStart", 0.1f, 0.9f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Top height variation", "cloudTopVariance", 0.0f, 0.6f);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Erosion"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Medium detail frequency", "cloudMediumRepeats", 1, 16);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Fine detail frequency", "cloudFineRepeats", 4, 64);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Medium erosion strength", "cloudMediumErosion", 0.0f, 0.5f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fine erosion strength", "cloudFineErosion", 0.0f, 0.3f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Fine detail distortion", "cloudWarpStrength", 0.0f, 2.0f);
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

                if (ImGui::TreeNode("Wind"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind X (m/s)", "cloudWindX", -50.0f, 50.0f);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderFloat("Wind Z (m/s)", "cloudWindZ", -50.0f, 50.0f);
                    ImGui::TreePop();
                }

                if (ImGui::TreeNode("Quality"))
                {
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Light cache samples", "cloudLightSteps", 4, 32);
                    radianceSettingsChanged |= SettingsGuiHelpers::SliderUint("Broad ray samples", "cloudSecondarySteps", 4, 32);
                    const bool resolutionChanged = SettingsGuiHelpers::SliderUint("Primary resolution divisor", "cloudViewDownscale", 1, 4);
                    renderState.needsResize |= resolutionChanged;
                    radianceSettingsChanged |= resolutionChanged;
                    ImGui::TreePop();
                }
                if (ImGui::Button("Reset cloud defaults"))
                {
                    SettingsManager::setAsBool("clouds", true);
                    SettingsManager::setAsFloat("cloudCoverage", 0.3f);
                    SettingsManager::setAsFloat("cloudDensity", 0.025f);
                    SettingsManager::setAsUint("cloudSteps", 32u);
                    SettingsManager::setAsFloat("cloudBaseHeight", 650.0f);
                    SettingsManager::setAsFloat("cloudThickness", 450.0f);
                    SettingsManager::setAsFloat("cloudPeriod", 4096.0f);
                    SettingsManager::setAsFloat("cloudMaxDistance", 30000.0f);
                    SettingsManager::setAsUint("cloudWeatherScale", 1u);
                    SettingsManager::setAsUint("cloudBillowScale", 3u);
                    SettingsManager::setAsFloat("cloudPerlinWeight", 0.55f);
                    SettingsManager::setAsFloat("cloudShapeThreshold", 0.2f);
                    SettingsManager::setAsFloat("cloudShapeGain", 2.5f);
                    SettingsManager::setAsFloat("cloudWeatherSoftness", 0.025f);
                    SettingsManager::setAsFloat("cloudBottomFade", 0.1f);
                    SettingsManager::setAsFloat("cloudTopStart", 0.55f);
                    SettingsManager::setAsFloat("cloudTopVariance", 0.35f);
                    SettingsManager::setAsUint("cloudMediumRepeats", 4u);
                    SettingsManager::setAsUint("cloudFineRepeats", 16u);
                    SettingsManager::setAsFloat("cloudMediumErosion", 0.16f);
                    SettingsManager::setAsFloat("cloudFineErosion", 0.04f);
                    SettingsManager::setAsFloat("cloudWarpStrength", 0.4f);
                    SettingsManager::setAsFloat("cloudAmbient", 0.3f);
                    SettingsManager::setAsFloat("cloudPhaseG", 0.65f);
                    SettingsManager::setAsFloat("cloudMultiScatter", 1.0f);
                    SettingsManager::setAsFloat("cloudPowder", 1.0f);
                    SettingsManager::setAsFloat("cloudAerial", 2.5e-05f);
                    SettingsManager::setAsFloat("cloudWindX", 8.0f);
                    SettingsManager::setAsFloat("cloudWindZ", 3.0f);
                    SettingsManager::setAsUint("cloudLightSteps", 12u);
                    SettingsManager::setAsUint("cloudSecondarySteps", 8u);
                    SettingsManager::setAsUint("cloudViewDownscale", 2u);
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
                    text += " --cloudWeatherScale=" + std::to_string(SettingsManager::getAsUint("cloudWeatherScale"));
                    text += " --cloudBillowScale=" + std::to_string(SettingsManager::getAsUint("cloudBillowScale"));
                    text += " --cloudPerlinWeight=" + std::to_string(SettingsManager::getAsFloat("cloudPerlinWeight"));
                    text += " --cloudShapeThreshold=" + std::to_string(SettingsManager::getAsFloat("cloudShapeThreshold"));
                    text += " --cloudShapeGain=" + std::to_string(SettingsManager::getAsFloat("cloudShapeGain"));
                    text += " --cloudWeatherSoftness=" + std::to_string(SettingsManager::getAsFloat("cloudWeatherSoftness"));
                    text += " --cloudBottomFade=" + std::to_string(SettingsManager::getAsFloat("cloudBottomFade"));
                    text += " --cloudTopStart=" + std::to_string(SettingsManager::getAsFloat("cloudTopStart"));
                    text += " --cloudTopVariance=" + std::to_string(SettingsManager::getAsFloat("cloudTopVariance"));
                    text += " --cloudMediumRepeats=" + std::to_string(SettingsManager::getAsUint("cloudMediumRepeats"));
                    text += " --cloudFineRepeats=" + std::to_string(SettingsManager::getAsUint("cloudFineRepeats"));
                    text += " --cloudMediumErosion=" + std::to_string(SettingsManager::getAsFloat("cloudMediumErosion"));
                    text += " --cloudFineErosion=" + std::to_string(SettingsManager::getAsFloat("cloudFineErosion"));
                    text += " --cloudWarpStrength=" + std::to_string(SettingsManager::getAsFloat("cloudWarpStrength"));
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
