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

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();
    optionAdder("h,help", "Print this message");
    optionAdder("width", "Window width", cxxopts::value<uint32_t>()->default_value("1920"));
    optionAdder("height", "Window height", cxxopts::value<uint32_t>()->default_value("1080"));
    optionAdder("maxPathDepth", "Maximum path depth", cxxopts::value<uint32_t>()->default_value("12"));
    optionAdder("scene", "Scene file (*.gltf; *.glb)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("testOutput", "Test screenshot output path (*.png)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("samplingMode", "Sampling mode (0=naive, 1=MIS, 2=RIS)", cxxopts::value<uint32_t>()->default_value("2"));
    optionAdder("tonemapping", "Tonemapping (0=none, 1=standard, 2=agx, 3=khronos pbr neutral)", cxxopts::value<uint32_t>()->default_value("3"));
    optionAdder("antialiasingMode", "Antialiasing mode (0=none, 1=accumulate, 2=DLSS)", cxxopts::value<uint32_t>()->default_value("0"));
    optionAdder("maxAccumulatedFrames", "Max accumulated frames", cxxopts::value<uint32_t>()->default_value("512"));
    optionAdder("dlssMode", "DLSS mode", cxxopts::value<uint32_t>()->default_value("2")); // sl::DLSSMode::eBalanced
    optionAdder("doPathSplitting", "Enable path splitting", cxxopts::value<bool>()->default_value("true"));
    optionAdder("useVsync", "Enable VSync", cxxopts::value<bool>()->default_value("true"));
    optionAdder("lockCamera", "Lock camera (disable player input)", cxxopts::value<bool>()->default_value("false"));
    optionAdder("voxelMode", "Enable voxel mode", cxxopts::value<bool>()->default_value("false"));
    optionAdder("worldSeed", "World seed", cxxopts::value<uint32_t>()->default_value("1738"));
    optionAdder("movementSpeed", "Movement speed", cxxopts::value<float>()->default_value("12"));
    optionAdder("fullscreen", "Start in fullscreen mode", cxxopts::value<bool>()->default_value("false"));
    optionAdder("useWaitableSwapChain", "Use waitable swap chain", cxxopts::value<bool>()->default_value("true"));
    optionAdder("showGui", "Show GUI", cxxopts::value<bool>()->default_value("true"));
    optionAdder("refractionIndirectPassthrough", "Treat transmissive surfaces as passthrough after diffuse bounces", cxxopts::value<bool>()->default_value("false"));

    optionAdder("debugView", "Debug view", cxxopts::value<std::string>()->default_value("off"));
    optionAdder("debugViewScale", "Debug view scale", cxxopts::value<float>()->default_value("1.f"));
    optionAdder("debugColorChunks", "Color chunks", cxxopts::value<bool>()->default_value("false"));
    optionAdder("debugBool0", "Debug bool 0", cxxopts::value<bool>()->default_value("false"));
    optionAdder("debugBool1", "Debug bool 1", cxxopts::value<bool>()->default_value("false"));
    optionAdder("debugBool2", "Debug bool 2", cxxopts::value<bool>()->default_value("false"));
    optionAdder("debugBool3", "Debug bool 3", cxxopts::value<bool>()->default_value("false"));
    optionAdder("debugFloat0", "Debug float 0", cxxopts::value<float>()->default_value("0.f"));
    optionAdder("debugFloat1", "Debug float 1", cxxopts::value<float>()->default_value("0.f"));
    optionAdder("debugFloat2", "Debug float 2", cxxopts::value<float>()->default_value("0.f"));
    optionAdder("debugFloat3", "Debug float 3", cxxopts::value<float>()->default_value("0.f"));
    optionAdder("gpuValidation", "Enable GPU validation (debug mode only)", cxxopts::value<bool>()->default_value("false"));
    optionAdder("verboseLogging", "Enable SL verbose logging (debug mode only)", cxxopts::value<bool>()->default_value("false"));

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
            exit(-1);
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
        exit(-1);
    }

    if (getAsUint("antialiasingMode") >= static_cast<uint32_t>(AntialiasingMode::COUNT))
    {
        std::cerr << "Invalid antialiasingMode option" << std::endl;
        exit(-1);
    }

    if (getAsUint("tonemapping") >= static_cast<uint32_t>(Tonemapping::COUNT))
    {
        std::cerr << "Invalid tonemapping option" << std::endl;
        exit(-1);
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

std::string getAsString(const std::string& name)
{
    return std::get<std::string>(settings.at(name));
}

void setAsBool(const std::string& name, bool value)
{
    settings[name] = value;
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

} // namespace SettingsManager
