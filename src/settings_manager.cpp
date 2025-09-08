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
*/                                                                                                                     \

#include "settings_manager.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts.hpp>

#include <variant>
#include <unordered_map>

#include "rendering/common/common_structs.h"

namespace SettingsManager
{

using namespace cxxopts;

using settingValue = std::variant<bool, int, uint32_t, std::string>;
static std::unordered_map<std::string, settingValue> settings;

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();
    optionAdder("h,help", "Print this message");
    optionAdder("width", "Window width", cxxopts::value<uint32_t>()->default_value("1920"));
    optionAdder("height", "Window height", cxxopts::value<uint32_t>()->default_value("1080"));
    optionAdder("spp", "Samples per pixel", cxxopts::value<uint32_t>()->default_value("16"));
    optionAdder("maxPathDepth", "Maximum path depth", cxxopts::value<uint32_t>()->default_value("12"));
    optionAdder("scene", "Scene file (*.gltf; *.glb)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("testOutput", "Test screenshot output path (*.png)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("enableMis", "Enable MIS", cxxopts::value<bool>()->default_value("true"));
    optionAdder("tonemapping", "Tonemapping (0=none, 1=standard, 2=agx, 3=khronos pbr neutral)", cxxopts::value<uint32_t>()->default_value("3"));
    optionAdder("enableSer", "Enable SER", cxxopts::value<bool>()->default_value("false"));
    optionAdder("forceRecompileShaders", "Force recompile shaders", cxxopts::value<bool>()->default_value("false"));

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
    COPY_SETTING("spp", uint32_t);
    COPY_SETTING("maxPathDepth", uint32_t);
    COPY_SETTING("scene", std::string);
    COPY_SETTING("testOutput", std::string);
    COPY_SETTING("enableMis", bool);
    COPY_SETTING("tonemapping", uint32_t);
    COPY_SETTING("enableSer", bool);
    COPY_SETTING("forceRecompileShaders", bool);

#undef COPY_SETTING

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

std::string getAsString(const std::string& name)
{
    return std::get<std::string>(settings.at(name));
}

} // namespace SettingsManager
