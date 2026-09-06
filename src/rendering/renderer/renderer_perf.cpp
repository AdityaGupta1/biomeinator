// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "logger.h"
#include "rendering/common/common_enums.h"
#include "settings_manager.h"
#include "util/file_util.h"

#include <json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace Renderer
{

// Warmup also waits for this many consecutive frames with no scene change, so voxel worlds
// finish streaming boundary chunks before measuring starts
static constexpr uint32_t PERF_QUIET_FRAMES = 30;

static double secondsSince(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static void enterPhase(const PerfPhase phase)
{
    PerfRunState& perfRun = renderState.perfRun;
    perfRun.phase = phase;
    perfRun.phaseStartFrame = renderState.frameNumber;
    perfRun.phaseStartTime = std::chrono::steady_clock::now();
}

void perfRunInit()
{
    PerfRunState& perfRun = renderState.perfRun;
    perfRun.active = SettingsManager::isPerfMode();
    if (!perfRun.active)
    {
        return;
    }

    perfRun.startTime = std::chrono::steady_clock::now();
    enterPhase(PerfPhase::WAITING_FOR_SCENE);

    // Locks GPU clocks to base so runs are comparable; needs Windows developer mode
    const HRESULT hr = renderState.device->SetStablePowerState(TRUE);
    perfRun.stablePowerState = SUCCEEDED(hr);
    if (!perfRun.stablePowerState)
    {
        Logger::logWarning("perf run: SetStablePowerState failed (0x%08X); is developer mode enabled? "
                           "Timings will be noisier",
                           static_cast<unsigned>(hr));
    }
}

void perfRunUpdate(const bool sceneReady, const bool didSceneChange, const double deltaTime)
{
    PerfRunState& perfRun = renderState.perfRun;
    if (!perfRun.active || perfRun.phase == PerfPhase::DONE)
    {
        return;
    }

    perfRun.quietStreak = didSceneChange ? 0 : perfRun.quietStreak + 1;

    if (secondsSince(perfRun.startTime) > SettingsManager::getAsFloat("perfTimeoutSeconds"))
    {
        Logger::logWarning("perf run: timed out in phase %u", static_cast<uint32_t>(perfRun.phase));
        perfRun.timedOut = true;
        if (perfRun.phase != PerfPhase::MEASURING)
        {
            perfRun.measureStartFrame = renderState.frameNumber; // empty range: nothing was measured
        }
        perfRun.measureEndFrame = renderState.frameNumber;
        enterPhase(PerfPhase::DONE);
        return;
    }

    const uint32_t framesInPhase = renderState.frameNumber - perfRun.phaseStartFrame;
    switch (perfRun.phase)
    {
        case PerfPhase::WAITING_FOR_SCENE:
            if (sceneReady)
            {
                Logger::log("perf run: scene ready at frame %u, warming up", renderState.frameNumber);
                enterPhase(PerfPhase::WARMUP);
            }
            break;

        case PerfPhase::WARMUP:
            if (framesInPhase >= SettingsManager::getAsUint("perfWarmupFrames") &&
                secondsSince(perfRun.phaseStartTime) >= SettingsManager::getAsFloat("perfWarmupSeconds") &&
                perfRun.quietStreak >= PERF_QUIET_FRAMES)
            {
                Logger::log("perf run: measuring from frame %u", renderState.frameNumber);
                perfRun.measureStartFrame = renderState.frameNumber;
                enterPhase(PerfPhase::MEASURING);
                perfRun.cpuFrameMs.push_back(deltaTime * 1000.0);
            }
            break;

        case PerfPhase::MEASURING:
            if (framesInPhase >= SettingsManager::getAsUint("perfFrames"))
            {
                perfRun.measureEndFrame = renderState.frameNumber;
                enterPhase(PerfPhase::DONE);
            }
            else
            {
                perfRun.cpuFrameMs.push_back(deltaTime * 1000.0);
            }
            break;

        case PerfPhase::DONE:
            break;
    }
}

void perfRunOnFrameTimings(const GpuProfiler::FrameTimings& timings)
{
    PerfRunState& perfRun = renderState.perfRun;
    if (!perfRun.active || perfRun.phase == PerfPhase::WAITING_FOR_SCENE || perfRun.phase == PerfPhase::WARMUP)
    {
        return;
    }

    const bool measured = timings.frameNumber >= perfRun.measureStartFrame &&
                          (perfRun.phase == PerfPhase::MEASURING || timings.frameNumber < perfRun.measureEndFrame);
    if (measured)
    {
        perfRun.gpuSamples.push_back(timings);
    }
}

bool perfRunIsDone()
{
    return renderState.perfRun.active && renderState.perfRun.phase == PerfPhase::DONE;
}

static nlohmann::json statsJson(std::vector<double> samples)
{
    if (samples.empty())
    {
        return nullptr;
    }
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](const double p)
    {
        const size_t idx = std::min(samples.size() - 1, static_cast<size_t>(p * static_cast<double>(samples.size())));
        return samples[idx];
    };
    double sum = 0.0;
    for (const double sample : samples)
    {
        sum += sample;
    }
    return {
        { "count", samples.size() },
        { "median", percentile(0.5) },
        { "mean", sum / static_cast<double>(samples.size()) },
        { "min", samples.front() },
        { "max", samples.back() },
        { "p95", percentile(0.95) },
    };
}

static nlohmann::json settingsJson()
{
    nlohmann::json json;
    SettingsManager::forEachSetting([&json](const std::string& name, const SettingsManager::SettingValue& value)
                                    { std::visit([&json, &name](const auto& v) { json[name] = v; }, value); });
    return json;
}

static nlohmann::json buildResultsJson()
{
    const PerfRunState& perfRun = renderState.perfRun;

    // Scopes are keyed by depth and name in order of first appearance, since not every scope
    // runs every frame (e.g. BLAS builds)
    struct ScopeSamples
    {
        const char* name;
        uint32_t depth;
        std::vector<double> ms;
    };
    std::vector<ScopeSamples> scopeSamples;
    std::map<std::pair<uint32_t, std::string>, size_t> scopeIdxByKey;
    std::vector<double> gpuFrameMs;
    gpuFrameMs.reserve(perfRun.gpuSamples.size());
    for (const GpuProfiler::FrameTimings& frame : perfRun.gpuSamples)
    {
        gpuFrameMs.push_back(frame.totalMs);
        for (const GpuProfiler::ScopeTiming& scope : frame.scopes)
        {
            const auto key = std::make_pair(scope.depth, std::string(scope.name));
            auto it = scopeIdxByKey.find(key);
            if (it == scopeIdxByKey.end())
            {
                it = scopeIdxByKey.emplace(key, scopeSamples.size()).first;
                scopeSamples.push_back({ .name = scope.name, .depth = scope.depth, .ms = {} });
            }
            scopeSamples[it->second].ms.push_back(scope.ms);
        }
    }

    nlohmann::json scopesJson = nlohmann::json::array();
    for (const ScopeSamples& scope : scopeSamples)
    {
        scopesJson.push_back({
            { "name", scope.name },
            { "depth", scope.depth },
            { "ms", statsJson(scope.ms) },
        });
    }

    const std::string sceneName =
        renderState.voxelMode ? SettingsManager::getAsString("world") : SettingsManager::getAsString("scene");
    return {
        { "meta",
          {
              { "scene", sceneName },
              { "adapter", renderState.adapterName },
              { "width", SettingsManager::getAsUint("width") },
              { "height", SettingsManager::getAsUint("height") },
              { "renderWidth", renderState.renderWidth },
              { "renderHeight", renderState.renderHeight },
              { "measuredFrames", perfRun.gpuSamples.size() },
              { "measureStartFrame", perfRun.measureStartFrame },
              { "stablePowerState", perfRun.stablePowerState },
              { "timedOut", perfRun.timedOut },
              { "timestamp", FileUtil::getTimestampString() },
          } },
        { "settings", settingsJson() },
        { "cpu", { { "frameMs", statsJson(perfRun.cpuFrameMs) } } },
        { "gpu", { { "frameMs", statsJson(gpuFrameMs) }, { "scopes", scopesJson } } },
    };
}

void perfRunFinish()
{
    PerfRunState& perfRun = renderState.perfRun;

    // The last NUM_FRAMES_IN_FLIGHT measured frames are still in flight
    flush();
    for (uint32_t slotIdx = 0; slotIdx < NUM_FRAMES_IN_FLIGHT; ++slotIdx)
    {
        GpuProfiler::FrameTimings timings;
        if (GpuProfiler::collect(slotIdx, timings))
        {
            perfRunOnFrameTimings(timings);
        }
    }

    const std::filesystem::path path = std::filesystem::absolute(SettingsManager::getAsString("perfOutput"));
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << buildResultsJson().dump(2);
    file.close();

    const bool ok = !perfRun.gpuSamples.empty();
    if (ok)
    {
        Logger::log(
            "perf run: wrote %zu measured frames to %s", perfRun.gpuSamples.size(), path.generic_string().c_str());
    }
    else
    {
        Logger::logError("perf run: no frames measured; wrote %s anyway", path.generic_string().c_str());
    }

    Renderer::destroy();
    exit(ok ? 0 : 1);
}

} // namespace Renderer
