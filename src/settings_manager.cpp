/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "settings_manager.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts.hpp>

#include <variant>
#include <unordered_map>

#include "rendering/common/common_enums.h"

namespace SettingsManager
{

using namespace cxxopts;

using settingValue = std::variant<bool, int, uint32_t, float, std::string>;
static std::unordered_map<std::string, settingValue> settings;

uint32_t worldSeed; // cached due to frequent access

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();

#define ADD_OPTION(name, desc, type, defaultValue) optionAdder(name, desc, cxxopts::value<type>()->default_value(defaultValue))

    optionAdder("h,help", "Print this message");
    ADD_OPTION("width", "Window width", uint32_t, "1920");
    ADD_OPTION("height", "Window height", uint32_t, "1080");
    ADD_OPTION("maxPathDepth", "Maximum path depth", uint32_t, "12");
    ADD_OPTION("scene", "Scene file (*.gltf; *.glb)", std::string, "");
    ADD_OPTION("testOutput", "Test screenshot output path (*.png)", std::string, "");
    ADD_OPTION("samplingMode", "Sampling mode (0=naive, 1=MIS, 2=RIS)", uint32_t, "2");
    ADD_OPTION("tonemapping", "Tonemapping (0=none, 1=standard, 2=agx, 3=khronos pbr neutral)", uint32_t, "3");
    ADD_OPTION("antialiasingMode", "Antialiasing mode (0=none, 1=accumulate, 2=DLSS)", uint32_t, "0");
    ADD_OPTION("maxAccumulatedFrames", "Max accumulated frames", uint32_t, "512");
    ADD_OPTION("dlssMode", "DLSS mode", uint32_t, "2"); // sl::DLSSMode::eBalanced
    ADD_OPTION("doPathSplitting", "Enable path splitting", bool, "true");
    ADD_OPTION("useVsync", "Enable VSync", bool, "true");
    ADD_OPTION("lockCamera", "Lock camera (disable player input)", bool, "false");
    ADD_OPTION("voxelMode", "Enable voxel mode", bool, "false");
    ADD_OPTION("worldSeed", "World seed", uint32_t, "1738");
    ADD_OPTION("movementSpeed", "Movement speed", float, "12");
    ADD_OPTION("fullscreen", "Start in fullscreen mode", bool, "false");
    ADD_OPTION("useWaitableSwapChain", "Use waitable swap chain", bool, "true");
    ADD_OPTION("showGui", "Show GUI", bool, "true");
    ADD_OPTION("refractionIndirectPassthrough", "Treat transmissive surfaces as passthrough after diffuse bounces", bool, "true");
    ADD_OPTION("renderDistance", "Render distance in chunks", int, "30");

    ADD_OPTION("rcEnabled", "Enable radiance cache", bool, "true");
    ADD_OPTION("rcMinSamplesForQuery", "Min samples before querying radiance cache", uint32_t, "4");

    ADD_OPTION("debugView", "Debug view", std::string, "off");
    ADD_OPTION("debugViewScale", "Debug view scale", float, "1.f");
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

#define COPY_SETTING(name, type) settings[name] = parseResult[name].as<type>()

    COPY_SETTING("width", uint32_t);
    COPY_SETTING("height", uint32_t);
    COPY_SETTING("maxPathDepth", uint32_t);
    COPY_SETTING("scene", std::string);
    COPY_SETTING("testOutput", std::string);
    COPY_SETTING("samplingMode", uint32_t);
    COPY_SETTING("tonemapping", uint32_t);
    COPY_SETTING("antialiasingMode", uint32_t);
    COPY_SETTING("maxAccumulatedFrames", uint32_t);
    COPY_SETTING("dlssMode", uint32_t);
    COPY_SETTING("doPathSplitting", bool);
    COPY_SETTING("useVsync", bool);
    COPY_SETTING("lockCamera", bool);
    COPY_SETTING("voxelMode", bool);
    COPY_SETTING("worldSeed", uint32_t);
    COPY_SETTING("movementSpeed", float);
    COPY_SETTING("fullscreen", bool);
    COPY_SETTING("useWaitableSwapChain", bool);
    COPY_SETTING("showGui", bool);
    COPY_SETTING("refractionIndirectPassthrough", bool);
    COPY_SETTING("renderDistance", int);

    COPY_SETTING("rcEnabled", bool);
    COPY_SETTING("rcMinSamplesForQuery", uint32_t);

    COPY_SETTING("debugView", std::string);
    COPY_SETTING("debugViewScale", float);
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

    worldSeed = getAsUint("worldSeed");
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

std::string getAsString(const std::string& name)
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

} // namespace SettingsManager
