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
    imguiDX12InitInfo.Device = device.Get();
    imguiDX12InitInfo.CommandQueue = graphicsCmdQueue.Get();
    imguiDX12InitInfo.NumFramesInFlight = NUM_FRAMES_IN_FLIGHT;
    imguiDX12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    imguiDX12InitInfo.SrvDescriptorHeap = sharedDescriptorHeap.Get();
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
    didPathTracingSettingsChange = false;

    ImGui::SetNextWindowPos(ImVec2(10, 10));

    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("Settings", nullptr, windowFlags | ImGuiWindowFlags_AlwaysAutoResize))
    {
        didPathTracingSettingsChange |= SettingsGuiHelpers::InputUint("Max path depth", "maxPathDepth", 1, 16);
        SettingsGuiHelpers::ComboUint("Tonemapping", "tonemapping", tonemappingComboOptions);
        needsResize |= SettingsGuiHelpers::Checkbox("Enable path splitting", "doPathSplitting");

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Sampling");
        didPathTracingSettingsChange |= SettingsGuiHelpers::ComboUint("Sampling mode", "samplingMode", samplingModeComboOptions);

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Materials");
        didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Refraction indirect passthrough", "refractionIndirectPassthrough");

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Radiance Cache");
        const bool didNrcChange = SettingsGuiHelpers::Checkbox("Enable NRC", "nrcEnabled");
        didPathTracingSettingsChange |= didNrcChange;
        needsResize |= didNrcChange;

        if (ImGui::CollapsingHeader("NRC Settings"))
        {
            {
                int nrcResolveModeInt = static_cast<int>(SettingsManager::getAsUint("nrcResolveMode"));
                SettingsGuiHelpers::ScopedItemWidth width(SettingsGuiHelpers::comboWidth);
                if (ImGui::Combo("NRC resolve mode", &nrcResolveModeInt, nrc::GetImGuiResolveModeComboString()))
                {
                    SettingsManager::setAsUint("nrcResolveMode", static_cast<uint32_t>(nrcResolveModeInt));
                    didPathTracingSettingsChange = true;
                }
            }

            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Max radiance", "nrcMaxRadiance", 0.01f, 100.0f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Termination threshold", "nrcTerminationThreshold", 0.001f, 1.0f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Training termination threshold", "nrcTrainingTerminationThreshold", 0.001f, 1.0f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Skip delta vertices", "nrcSkipDeltaVertices");
            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Train the cache", "nrcTrainTheCache");
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Learning rate", "nrcLearningRate", 0.0001f, 0.1f);
        }

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("Antialiasing");
        const bool didAntialiasingChange = SettingsGuiHelpers::ComboUint("Antialiasing mode", "antialiasingMode", antialiasingModeComboOptions);
        needsResize |= didAntialiasingChange; // technically should need resize only when switching to or from DLSS, but whatever
        didPathTracingSettingsChange |= didAntialiasingChange;
        const AntialiasingMode antialiasingMode = static_cast<AntialiasingMode>(SettingsManager::getAsUint("antialiasingMode"));

        if (antialiasingMode == AntialiasingMode::ACCUMULATE)
        {
            ImGui::Text("accumulated frames: %u", accumulatedFrameNumber);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderUint("Max accumulated frames", "maxAccumulatedFrames", 1, 2048);
        }
        else if (antialiasingMode == AntialiasingMode::DLSS)
        {
            needsResize |= SettingsGuiHelpers::ComboUint("DLSS mode", "dlssMode", dlssModeOptions);
        }

        SettingsGuiHelpers::VerticalSpacing();
        SettingsGuiHelpers::SectionTitle("World");
        SettingsGuiHelpers::SliderFloat("Movement speed", "movementSpeed", 1.f, 100.f);

        SettingsGuiHelpers::VerticalSpacing();

        if (ImGui::CollapsingHeader("Debug", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SettingsGuiHelpers::SectionTitle("Debug view");
            SettingsGuiHelpers::ComboString("Debug view", "debugView", debugViewComboOptions);
            SettingsGuiHelpers::SliderFloat("Debug view scale", "debugViewScale", -1000.f, 1000.f);
            SettingsGuiHelpers::Checkbox("Debug view apply tonemap", "debugViewApplyTonemap");

            SettingsGuiHelpers::VerticalSpacing();

            if (voxelMode)
            {
                didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Color chunks", "debugColorChunks");
            }

            SettingsGuiHelpers::VerticalSpacing();

            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 0", "debugBool0");
            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 1", "debugBool1");
            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 2", "debugBool2");
            didPathTracingSettingsChange |= SettingsGuiHelpers::Checkbox("Debug bool 3", "debugBool3");
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 0", "debugFloat0", -100.f, 100.f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 1", "debugFloat1", -100.f, 100.f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 2", "debugFloat2", -100.f, 100.f);
            didPathTracingSettingsChange |= SettingsGuiHelpers::SliderFloat("Debug float 3", "debugFloat3", -100.f, 100.f);
        }
    }
    ImGui::End();

    constexpr int performanceWindowHeight = 240;
    ImGui::SetNextWindowPos(ImVec2(10, viewport.Height - 10 - performanceWindowHeight));
    ImGui::SetNextWindowSize(ImVec2(800, performanceWindowHeight));

    if (ImGui::Begin("Performance", nullptr, windowFlags))
    {
        ImGui::Text("FPS: %d", lastFps);

        SettingsGuiHelpers::VerticalSpacing();
        frameTimeBuffer.push({ static_cast<float>(frameNumber), static_cast<float>(deltaTime) * 1000.f });
        if (ImPlot::BeginPlot("Frame time", ImVec2(-1, -1)))
        {
            static constexpr ImPlotAxisFlags axisFlags = 0;
            ImPlot::SetupAxes(nullptr, nullptr, axisFlags, axisFlags);
            ImPlot::SetupAxisLimits(ImAxis_X1,
                                    static_cast<int>(frameNumber) - static_cast<int>(frameTimeBuffer.getMaxSize()),
                                    frameNumber,
                                    ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 20);
            ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.5f);
            ImPlot::PlotShaded("Frame time",
                               &frameTimeBuffer.getData()[0].frameIdx,
                               &frameTimeBuffer.getData()[0].timeMs,
                               static_cast<int>(frameTimeBuffer.getSize()),
                               -INFINITY,
                               ImPlotItemFlags_NoLegend,
                               frameTimeBuffer.getOffset(),
                               sizeof(FrameTimeMeasurement));
            ImPlot::EndPlot();
        }
    }
    ImGui::End();

    constexpr int debugWindowWidth = 300;
    ImGui::SetNextWindowPos(ImVec2(viewport.Width - 10 - debugWindowWidth, 10));
    ImGui::SetNextWindowSize(ImVec2(debugWindowWidth, -1));

    if (ImGui::Begin("Debug", nullptr, windowFlags))
    {
        const glm::vec3 cameraPos_WS = camera.getPos_WS();
        ImGui::Text("Position: (%.2f, %.2f, %.2f)", cameraPos_WS.x, cameraPos_WS.y, cameraPos_WS.z);
    }
    ImGui::End();

    if (frameNumber == 0)
    {
        ImGui::SetWindowFocus(NULL);
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList.Get());
}

} // namespace Renderer
