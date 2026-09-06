// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include "logger.h"
#include "settings_manager.h"
#include "util/file_util.h"

#include <json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>

namespace Renderer
{

// Warmup also waits for this many consecutive frames with no scene change, so voxel worlds
// finish streaming boundary chunks before measuring starts
static constexpr uint32_t PERF_QUIET_FRAMES = 30;

static const char* phaseName(const PerfPhase phase)
{
    switch (phase)
    {
        case PerfPhase::WAITING_FOR_SCENE:
            return "waiting for scene";
        case PerfPhase::WARMUP:
            return "warmup";
        case PerfPhase::MEASURING:
            return "measuring";
        case PerfPhase::DONE:
            return "done";
    }
    return "unknown";
}

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

// SetStablePowerState needs Windows developer mode; without it the call fails and then removes
// the device, so it must not even be attempted
static bool isDeveloperModeEnabled()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                                        L"AllowDevelopmentWithoutDevLicense",
                                        RRF_RT_REG_DWORD,
                                        nullptr,
                                        &value,
                                        &size);
    return status == ERROR_SUCCESS && value != 0;
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

    // Locks GPU clocks to base so runs are comparable
    if (isDeveloperModeEnabled())
    {
        CHECK_HRESULT(renderState.device->SetStablePowerState(TRUE));
        perfRun.stablePowerState = true;
    }
    else
    {
        Logger::logWarning("perf run: developer mode is off, so GPU clocks are not locked; timings will be noisier");
    }
}

void perfRunUpdate(const bool sceneReady, const bool didSceneChange)
{
    PerfRunState& perfRun = renderState.perfRun;
    if (!perfRun.active || perfRun.phase == PerfPhase::DONE)
    {
        return;
    }

    perfRun.quietStreak = didSceneChange ? 0 : perfRun.quietStreak + 1;

    if (secondsSince(perfRun.startTime) > SettingsManager::getAsFloat("perfTimeoutSeconds"))
    {
        Logger::logWarning("perf run: timed out in phase '%s'", phaseName(perfRun.phase));
        perfRun.timedOut = true;
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
            }
            break;

        case PerfPhase::MEASURING:
            if (framesInPhase >= SettingsManager::getAsUint("perfFrames"))
            {
                perfRun.measureEndFrame = renderState.frameNumber;
                enterPhase(PerfPhase::DONE);
            }
            break;

        case PerfPhase::DONE:
            break;
    }
}

void perfRunBeginCpuFrame()
{
    if (renderState.perfRun.active)
    {
        renderState.perfRun.cpuFrameStart = std::chrono::steady_clock::now();
    }
}

void perfRunEndCpuFrame()
{
    PerfRunState& perfRun = renderState.perfRun;
    // perfRunUpdate has already run this frame, so the phase says whether this frame number is
    // inside the measured range; that keeps the CPU samples paired with the GPU ones
    if (perfRun.phase == PerfPhase::MEASURING)
    {
        perfRun.cpuFrameMs.push_back(secondsSince(perfRun.cpuFrameStart) * 1000.0);
    }
}

void perfRunCollectTimings(const uint32_t slotIdx)
{
    GpuProfiler::FrameTimings timings;
    if (!GpuProfiler::collect(slotIdx, timings))
    {
        return;
    }

    PerfRunState& perfRun = renderState.perfRun;
    if (perfRun.active && timings.frameNumber >= perfRun.measureStartFrame &&
        timings.frameNumber < perfRun.measureEndFrame)
    {
        perfRun.gpuSamples.push_back(std::move(timings));
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

// Shift outcome counters accumulated over the whole run (warmup included), see RESTIR_STATS_* in
// common_structs.h. Null unless --restirShiftStats was set.
static nlohmann::json restirShiftStatsJson()
{
    if (!SettingsManager::getAsBool("restirShiftStats"))
    {
        return nullptr;
    }
    if (!RESTIR_SHIFT_STATS)
    {
        Logger::logError("perf run: --restirShiftStats needs a build with RESTIR_SHIFT_STATS set; no stats recorded");
        return nullptr;
    }

    std::array<uint32_t, RESTIR_STATS_COUNT> counters{};
    const D3D12_RANGE readRange = { 0, sizeof(counters) };
    void* mapped = nullptr;
    CHECK_HRESULT(renderState.dev_restirStatsReadback->Map(0, &readRange, &mapped));
    memcpy(counters.data(), mapped, sizeof(counters));
    const D3D12_RANGE emptyRange = { 0, 0 };
    renderState.dev_restirStatsReadback->Unmap(0, &emptyRange);

    nlohmann::json json;
    const std::pair<const char*, uint32_t> passes[] = { { "spatial", RESTIR_STATS_SPATIAL_BASE }, { "temporal", RESTIR_STATS_TEMPORAL_BASE } };
    for (const auto& [passName, base] : passes)
    {
        nlohmann::json attempted = nlohmann::json::array();
        nlohmann::json succeeded = nlohmann::json::array();
        for (uint32_t bucket = 0; bucket < RESTIR_STATS_REPLAY_BUCKETS; ++bucket)
        {
            attempted.push_back(counters[base + RESTIR_STATS_BUCKETS_BASE + 2 * bucket]);
            succeeded.push_back(counters[base + RESTIR_STATS_BUCKETS_BASE + 2 * bucket + 1]);
        }
        json[passName] = {
            { "pairs", counters[base + RESTIR_STATS_PAIRS] },
            { "skippedEmpty", counters[base + RESTIR_STATS_SKIPPED] },
            { "skippedNoPartner", counters[base + RESTIR_STATS_NO_PARTNER] },
            { "attemptedByReplayVertices", attempted },
            { "succeededByReplayVertices", succeeded },
        };
    }
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
        { "restirShiftStats", restirShiftStatsJson() },
    };
}

void perfRunFinish()
{
    const PerfRunState& perfRun = renderState.perfRun;

    // The last NUM_FRAMES_IN_FLIGHT measured frames are still in flight
    flush();
    for (uint32_t slotIdx = 0; slotIdx < NUM_FRAMES_IN_FLIGHT; ++slotIdx)
    {
        perfRunCollectTimings(slotIdx);
    }

    const std::filesystem::path path = std::filesystem::absolute(SettingsManager::getAsString("perfOutput"));
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path);
    file << buildResultsJson().dump(2);
    file.close();

    if (perfRun.timedOut)
    {
        Logger::logError("perf run: timed out with %zu of %u frames measured; wrote %s anyway",
                         perfRun.gpuSamples.size(),
                         SettingsManager::getAsUint("perfFrames"),
                         path.generic_string().c_str());
    }
    else
    {
        Logger::log(
            "perf run: wrote %zu measured frames to %s", perfRun.gpuSamples.size(), path.generic_string().c_str());
    }

    Renderer::destroy();
    exit(perfRun.timedOut ? 1 : 0);
}

} // namespace Renderer
