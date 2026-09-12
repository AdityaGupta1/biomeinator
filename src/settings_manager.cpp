// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "settings_manager.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts.hpp>

#include <unordered_map>
#include <variant>

#include "rendering/common/common_enums.h"

namespace SettingsManager
{

using namespace cxxopts;

static std::unordered_map<std::string, SettingValue> settings;

uint32_t worldSeed; // cached due to frequent access

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();

#define ADD_OPTION(name, desc, type, defaultValue)                                                                     \
    optionAdder(name, desc, cxxopts::value<type>()->default_value(defaultValue))

    optionAdder("h,help", "Print this message");
    ADD_OPTION("width", "Window width", uint32_t, "1920");
    ADD_OPTION("height", "Window height", uint32_t, "1080");
    ADD_OPTION("rngSeed", "Fixed sampling seed (0=random)", uint32_t, "0");
    ADD_OPTION("sharc", "Enable spatial hash radiance cache", bool, "true");
    ADD_OPTION("sharcCapacityLog2", "SHARC cache capacity exponent (16-24)", uint32_t, "20");
    ADD_OPTION("sharcDownscale", "SHARC update pixel stride (1-16)", uint32_t, "5");
    ADD_OPTION("sharcSceneScale", "SHARC world-space grid scale", float, "50");
    ADD_OPTION("sharcRoughnessMin", "Minimum update-path roughness", float, "0.4");
    ADD_OPTION("sharcAccumulationFrames", "SHARC history length", uint32_t, "32");
    ADD_OPTION("sharcStaleFrames", "SHARC eviction age", uint32_t, "64");
    ADD_OPTION("sharcWarmupFrames", "Cache warmup before screenshot accumulation", uint32_t, "64");
    ADD_OPTION("sharcDebug", "SHARC view: 0 beauty, 1 hits, 2 bounces, 3 grid, 4 cached radiance", uint32_t, "0");
    ADD_OPTION("maxPathDepth", "Maximum path depth", uint32_t, "12");
    ADD_OPTION("scene", "Scene file (*.gltf; *.glb)", std::string, "");
    ADD_OPTION("testOutput", "Test screenshot output path (*.png)", std::string, "");
    ADD_OPTION("perfOutput", "Performance measurement output path (*.json)", std::string, "");
    ADD_OPTION("perfWarmupFrames", "Perf run: minimum frames before measuring starts", uint32_t, "100");
    ADD_OPTION("perfWarmupSeconds", "Perf run: minimum seconds before measuring starts", float, "2");
    ADD_OPTION("perfFrames", "Perf run: number of frames to measure", uint32_t, "300");
    ADD_OPTION("perfTimeoutSeconds", "Perf run: give up and write whatever was measured after this long", float, "120");
    ADD_OPTION("samplingMode", "Sampling mode (0=naive, 1=MIS, 2=RTSL)", uint32_t, "2");
    ADD_OPTION("tonemapping", "Tonemapping (0=none, 1=standard, 2=agx, 3=khronos pbr neutral)", uint32_t, "3");
    ADD_OPTION("antialiasingMode", "Antialiasing mode (0=none, 1=accumulate, 2=DLSS; defaults to DLSS in voxel mode)", uint32_t, "0");
    ADD_OPTION("maxAccumulatedFrames", "Max accumulated frames", uint32_t, "512");
    ADD_OPTION("dlssMode", "DLSS mode", uint32_t, "2"); // sl::DLSSMode::eBalanced
    ADD_OPTION("dlssPreset", "DLSS Ray Reconstruction preset (0=D, 1=F)", uint32_t, "1");
    ADD_OPTION("frameGeneration", "Enable DLSS frame generation", bool, "true");
    ADD_OPTION("doPathSplitting", "Enable path splitting", bool, "true");
    ADD_OPTION("useVsync", "Enable VSync", bool, "false");
    ADD_OPTION("lockCamera", "Lock camera (disable player input)", bool, "false");
    ADD_OPTION("noJitter", "Disable jitter", bool, "false");
    ADD_OPTION("voxelMode", "Enable voxel mode", bool, "false");
    ADD_OPTION("worldSeed", "World seed", uint32_t, "1738");
    ADD_OPTION("movementSpeed", "Movement speed", float, "12");
    ADD_OPTION("animTimePaused", "Pause world animation (e.g. water waves, sun position)", bool, "false");
    ADD_OPTION("animTime", "Initial world animation time in seconds (0 = sunrise)", float, "150");
    ADD_OPTION("fullscreen", "Start in fullscreen mode", bool, "false");
    ADD_OPTION("useWaitableSwapChain", "Use waitable swap chain", bool, "true");
    ADD_OPTION("showGui", "Show GUI", bool, "true");
    ADD_OPTION("refractionIndirectPassthrough", "Treat transmissive surfaces as passthrough after diffuse bounces", bool, "true");
    ADD_OPTION("fogScatteringMultiplier", "Fog scattering multiplier on the time-of-day fog strength (0 disables fog; voxel mode only)", float, "1");
    ADD_OPTION("fogScaleHeight", "Fog density falloff scale height in blocks above sea level", float, "40");
    ADD_OPTION("fogG", "Fog Henyey-Greenstein anisotropy", float, "0.5");
    ADD_OPTION("fogMarchSteps", "Fog in-scattering march steps on the primary segment", uint32_t, "8");
    ADD_OPTION("fogAmbientStrength", "Strength of the fog ambient sky in-scattering term", float, "0.3");
    ADD_OPTION("clouds", "World-space volumetric clouds (voxel mode only)", bool, "true");
    ADD_OPTION("cloudCoverage", "Cloud coverage", float, "0.5");
    ADD_OPTION("cloudDensity", "Cloud extinction per meter", float, "0.007");
    ADD_OPTION("cloudSteps", "Cloud view ray march steps", uint32_t, "128");
    ADD_OPTION("cloudBaseHeight", "Cloud base (m)", float, "1500");
    ADD_OPTION("cloudThickness", "Layer thickness (m)", float, "1500");
    ADD_OPTION("cloudPeriod", "Weather tile size (m)", float, "55368.347656");
    ADD_OPTION("cloudMaxDistance", "Cloud draw distance (m)", float, "100000");
    ADD_OPTION("cloudWarpScale", "Distortion noise scale", float, "2.5");
    ADD_OPTION("cloudWarpDetail", "Distortion noise detail", float, "2");
    ADD_OPTION("cloudWarpRoughness", "Distortion noise roughness", float, "0.5");
    ADD_OPTION("cloudWarpStrength", "Distortion strength", float, "1");
    ADD_OPTION("cloudVoronoiSmoothness", "Voronoi Smooth F1 smoothness", float, "1");
    ADD_OPTION("cloudVoronoiRandomness", "Voronoi randomness", float, "0.666667");
    ADD_OPTION("cloudFineScale", "Detail noise scale", float, "30");
    ADD_OPTION("cloudFineDetail", "Detail noise octaves", float, "2");
    ADD_OPTION("cloudFineRoughness", "Detail noise roughness", float, "0.5");
    ADD_OPTION("cloudFineStrength", "Detail noise strength", float, "0.1");
    ADD_OPTION("cloudHeightRampEnd", "Height ramp white position", float, "0.177273");
    ADD_OPTION("cloudHeightGain", "Height influence", float, "6.1");
    ADD_OPTION("cloudDensityRampStart", "Density ramp white position", float, "0.145455");
    ADD_OPTION("cloudDensityRampEnd", "Density ramp black position", float, "0.363636");
    ADD_OPTION("cloudAmbient", "Sky ambient strength", float, "0.68");
    ADD_OPTION("cloudPhaseG", "Forward scattering", float, "0.65");
    ADD_OPTION("cloudMultiScatter", "Multiple scattering", float, "1");
    ADD_OPTION("cloudPowder", "Powder effect", float, "1");
    ADD_OPTION("cloudAerial", "Distance haze", float, "2.5e-05");
    ADD_OPTION("cloudWindX", "Wind X (m/s)", float, "8");
    ADD_OPTION("cloudWindZ", "Wind Z (m/s)", float, "3");
    ADD_OPTION("cloudLightSteps", "Light cache samples", uint32_t, "12");
    ADD_OPTION("cloudSecondarySteps", "Broad ray samples", uint32_t, "8");
    ADD_OPTION("cloudViewDownscale", "Primary resolution divisor", uint32_t, "2");
    ADD_OPTION("cloudWarpTime", "Distortion time offset", float, "0");
    ADD_OPTION("cloudWarpSpeed", "Distortion evolution speed", float, "0.02");
    ADD_OPTION("cloudVoronoiTime", "Cell time offset", float, "0");
    ADD_OPTION("cloudVoronoiSpeed", "Cell evolution speed", float, "0.01");
    ADD_OPTION("cloudFineTime", "Detail time offset", float, "0");
    ADD_OPTION("cloudFineSpeed", "Detail evolution speed", float, "0.05");
    ADD_OPTION("renderDistance", "Render distance in chunks", int, "30");
    ADD_OPTION("world", "World to import", std::string, "");

    ADD_OPTION("debugView", "Debug view", std::string, "off");
    ADD_OPTION("debugViewScale", "Debug view scale", float, "1.f");
    ADD_OPTION("debugViewApplyTonemap", "Apply tonemapping to debug output", bool, "false");
    ADD_OPTION("debugColorChunks", "Color chunks", bool, "false");
    ADD_OPTION("debugBool0", "Debug bool 0", bool, "false");
    ADD_OPTION("debugBool1", "Debug bool 1", bool, "false");
    ADD_OPTION("debugBool2", "Debug bool 2", bool, "false");
    ADD_OPTION("debugBool3", "Debug bool 3", bool, "false");
    ADD_OPTION("debugFloat0", "Debug float 0", float, "0.f");
    ADD_OPTION("debugFloat1", "Debug float 1", float, "0.f");
    ADD_OPTION("debugFloat2", "Debug float 2", float, "0.f");
    ADD_OPTION("debugFloat3", "Debug float 3", float, "0.f");
    ADD_OPTION("gpuValidation", "Enable GPU validation (debug mode only)", bool, "false");
    ADD_OPTION("verboseLogging", "Enable SL verbose logging (debug mode only)", bool, "false");

#undef ADD_OPTION

    ParseResult parseResult = options.parse(argc, argv);

    if (parseResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    if (parseResult.contains("testOutput"))
    {
        const std::string& testOutputPath = parseResult["testOutput"].as<std::string>();
        if (!testOutputPath.ends_with(".png"))
        {
            std::cerr << "--testOutput must be a .png" << std::endl;
            exit(1);
        }
    }

    if (parseResult.contains("perfOutput"))
    {
        const std::string& perfOutputPath = parseResult["perfOutput"].as<std::string>();
        if (!perfOutputPath.ends_with(".json"))
        {
            std::cerr << "--perfOutput must be a .json" << std::endl;
            exit(1);
        }
        if (parseResult.contains("testOutput"))
        {
            std::cerr << "--perfOutput and --testOutput are mutually exclusive" << std::endl;
            exit(1);
        }
    }

#define COPY_SETTING(name, type) settings[name] = parseResult[name].as<type>()

    COPY_SETTING("width", uint32_t);
    COPY_SETTING("height", uint32_t);
    COPY_SETTING("rngSeed", uint32_t);
    COPY_SETTING("sharc", bool);
    COPY_SETTING("sharcCapacityLog2", uint32_t);
    COPY_SETTING("sharcDownscale", uint32_t);
    COPY_SETTING("sharcSceneScale", float);
    COPY_SETTING("sharcRoughnessMin", float);
    COPY_SETTING("sharcAccumulationFrames", uint32_t);
    COPY_SETTING("sharcStaleFrames", uint32_t);
    COPY_SETTING("sharcWarmupFrames", uint32_t);
    COPY_SETTING("sharcDebug", uint32_t);
    COPY_SETTING("maxPathDepth", uint32_t);
    COPY_SETTING("scene", std::string);
    COPY_SETTING("testOutput", std::string);
    COPY_SETTING("perfOutput", std::string);
    COPY_SETTING("perfWarmupFrames", uint32_t);
    COPY_SETTING("perfWarmupSeconds", float);
    COPY_SETTING("perfFrames", uint32_t);
    COPY_SETTING("perfTimeoutSeconds", float);
    COPY_SETTING("samplingMode", uint32_t);
    COPY_SETTING("tonemapping", uint32_t);
    COPY_SETTING("antialiasingMode", uint32_t);
    COPY_SETTING("maxAccumulatedFrames", uint32_t);
    COPY_SETTING("dlssMode", uint32_t);
    COPY_SETTING("dlssPreset", uint32_t);
    COPY_SETTING("frameGeneration", bool);
    COPY_SETTING("doPathSplitting", bool);
    COPY_SETTING("useVsync", bool);
    COPY_SETTING("lockCamera", bool);
    COPY_SETTING("noJitter", bool);
    COPY_SETTING("voxelMode", bool);
    COPY_SETTING("worldSeed", uint32_t);
    COPY_SETTING("movementSpeed", float);
    COPY_SETTING("animTimePaused", bool);
    COPY_SETTING("animTime", float);
    COPY_SETTING("fullscreen", bool);
    COPY_SETTING("useWaitableSwapChain", bool);
    COPY_SETTING("showGui", bool);
    COPY_SETTING("refractionIndirectPassthrough", bool);
    COPY_SETTING("fogScatteringMultiplier", float);
    COPY_SETTING("fogScaleHeight", float);
    COPY_SETTING("fogG", float);
    COPY_SETTING("fogMarchSteps", uint32_t);
    COPY_SETTING("fogAmbientStrength", float);
    COPY_SETTING("clouds", bool);
    COPY_SETTING("cloudCoverage", float);
    COPY_SETTING("cloudDensity", float);
    COPY_SETTING("cloudSteps", uint32_t);
    COPY_SETTING("cloudBaseHeight", float);
    COPY_SETTING("cloudThickness", float);
    COPY_SETTING("cloudPeriod", float);
    COPY_SETTING("cloudMaxDistance", float);
    COPY_SETTING("cloudWarpScale", float);
    COPY_SETTING("cloudWarpDetail", float);
    COPY_SETTING("cloudWarpRoughness", float);
    COPY_SETTING("cloudWarpStrength", float);
    COPY_SETTING("cloudVoronoiSmoothness", float);
    COPY_SETTING("cloudVoronoiRandomness", float);
    COPY_SETTING("cloudFineScale", float);
    COPY_SETTING("cloudFineDetail", float);
    COPY_SETTING("cloudFineRoughness", float);
    COPY_SETTING("cloudFineStrength", float);
    COPY_SETTING("cloudHeightRampEnd", float);
    COPY_SETTING("cloudHeightGain", float);
    COPY_SETTING("cloudDensityRampStart", float);
    COPY_SETTING("cloudDensityRampEnd", float);
    COPY_SETTING("cloudAmbient", float);
    COPY_SETTING("cloudPhaseG", float);
    COPY_SETTING("cloudMultiScatter", float);
    COPY_SETTING("cloudPowder", float);
    COPY_SETTING("cloudAerial", float);
    COPY_SETTING("cloudWindX", float);
    COPY_SETTING("cloudWindZ", float);
    COPY_SETTING("cloudLightSteps", uint32_t);
    COPY_SETTING("cloudSecondarySteps", uint32_t);
    COPY_SETTING("cloudViewDownscale", uint32_t);
    COPY_SETTING("cloudWarpTime", float);
    COPY_SETTING("cloudWarpSpeed", float);
    COPY_SETTING("cloudVoronoiTime", float);
    COPY_SETTING("cloudVoronoiSpeed", float);
    COPY_SETTING("cloudFineTime", float);
    COPY_SETTING("cloudFineSpeed", float);
    COPY_SETTING("renderDistance", int);
    COPY_SETTING("world", std::string);

    COPY_SETTING("debugView", std::string);
    COPY_SETTING("debugViewScale", float);
    COPY_SETTING("debugViewApplyTonemap", bool);
    COPY_SETTING("debugColorChunks", bool);
    COPY_SETTING("debugBool0", bool);
    COPY_SETTING("debugBool1", bool);
    COPY_SETTING("debugBool2", bool);
    COPY_SETTING("debugBool3", bool);
    COPY_SETTING("debugFloat0", float);
    COPY_SETTING("debugFloat1", float);
    COPY_SETTING("debugFloat2", float);
    COPY_SETTING("debugFloat3", float);
    COPY_SETTING("gpuValidation", bool);
    COPY_SETTING("verboseLogging", bool);

#undef COPY_SETTING

    if (getAsUint("samplingMode") >= static_cast<uint32_t>(SamplingMode::COUNT))
    {
        std::cerr << "Invalid samplingMode option" << std::endl;
        exit(1);
    }

    if (getAsUint("antialiasingMode") >= static_cast<uint32_t>(AntialiasingMode::COUNT))
    {
        std::cerr << "Invalid antialiasingMode option" << std::endl;
        exit(1);
    }

    if (getAsUint("tonemapping") >= static_cast<uint32_t>(Tonemapping::COUNT))
    {
        std::cerr << "Invalid tonemapping option" << std::endl;
        exit(1);
    }

    if (getAsUint("dlssPreset") >= 2)
    {
        std::cerr << "Invalid dlssPreset option" << std::endl;
        exit(1);
    }

    if (getAsUint("sharcCapacityLog2") < 16 || getAsUint("sharcCapacityLog2") > 24 || getAsUint("sharcDownscale") < 1 ||
        getAsUint("sharcDownscale") > 16 ||
        !(getAsFloat("sharcSceneScale") > 0.f && getAsFloat("sharcSceneScale") <= 10000.f) ||
        !(getAsFloat("sharcRoughnessMin") >= 0.f && getAsFloat("sharcRoughnessMin") <= 1.f) ||
        getAsUint("sharcDebug") > 4 || getAsUint("sharcAccumulationFrames") > 1024 ||
        getAsUint("sharcStaleFrames") < 8 || getAsUint("sharcStaleFrames") > 1024)
    {
        std::cerr << "Invalid SHARC settings" << std::endl;
        exit(1);
    }

    worldSeed = getAsUint("worldSeed");

    if (!getAsString("world").empty())
    {
        settings["voxelMode"] = true;
    }

    if (getAsBool("voxelMode") && parseResult.count("antialiasingMode") == 0)
    {
        settings["antialiasingMode"] = static_cast<uint32_t>(AntialiasingMode::DLSS);
    }

    // A headless run renders a fixed, unanimated viewpoint with no frame-rate cap, so golden
    // screenshots are reproducible and perf measurements are not throttled; each of these can
    // still be overridden explicitly
    if (isHeadless())
    {
        const auto defaultTo = [&parseResult](const char* name, const bool value)
        {
            if (parseResult.count(name) == 0)
            {
                settings[name] = value;
            }
        };
        defaultTo("sharc", false); // Existing goldens and perf baselines remain uncached.
        defaultTo("lockCamera", true);
        defaultTo("showGui", false);
        defaultTo("animTimePaused", true);
        defaultTo("useVsync", false);
    }
}

void forEachSetting(const std::function<void(const std::string& name, const SettingValue& value)>& callback)
{
    for (const auto& [name, value] : settings)
    {
        callback(name, value);
    }
}

bool getAsBool(const std::string& name)
{
    return std::get<bool>(settings.at(name));
}

int getAsInt(const std::string& name)
{
    return std::get<int>(settings.at(name));
}

uint32_t getAsUint(const std::string& name)
{
    return std::get<uint32_t>(settings.at(name));
}

float getAsFloat(const std::string& name)
{
    return std::get<float>(settings.at(name));
}

const std::string& getAsString(const std::string& name)
{
    return std::get<std::string>(settings.at(name));
}

void setAsBool(const std::string& name, bool value)
{
    settings[name] = value;
}

void toggleBool(const std::string& name)
{
    settings[name] = !std::get<bool>(settings[name]);
}

void setAsInt(const std::string& name, int value)
{
    settings[name] = value;
}

void setAsUint(const std::string& name, uint32_t value)
{
    settings[name] = value;
}

void setAsFloat(const std::string& name, float value)
{
    settings[name] = value;
}

void setAsString(const std::string& name, const std::string& value)
{
    settings[name] = value;
}

uint32_t getWorldSeed()
{
    return worldSeed;
}

void setWorldSeed(uint32_t value)
{
    worldSeed = value;
    settings["worldSeed"] = value;
}

bool isTestMode()
{
    return !getAsString("testOutput").empty();
}

bool isPerfMode()
{
    return !getAsString("perfOutput").empty();
}

bool isHeadless()
{
    return isTestMode() || isPerfMode();
}

} // namespace SettingsManager
