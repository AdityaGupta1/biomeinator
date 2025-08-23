#include "settings_manager.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts/cxxopts.hpp>

namespace SettingsManager
{

using namespace cxxopts;

ParseResult parseResult;

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator.exe", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();
    optionAdder("h,help", "Print this message");
    optionAdder("width", "Window width", cxxopts::value<uint32_t>()->default_value("1920"));
    optionAdder("height", "Window height", cxxopts::value<uint32_t>()->default_value("1080"));
    optionAdder("spp", "Samples per pixel", cxxopts::value<uint32_t>()->default_value("16"));
    optionAdder("maxPathDepth", "Maximum path depth", cxxopts::value<uint32_t>()->default_value("12"));
    optionAdder("scene", "Scene file (*.gltf; *.glb)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("testOutput", "Test screenshot output path (*.png)", cxxopts::value<std::string>()->default_value(""));
    optionAdder("enableMis", "Enable MIS", cxxopts::value<bool>()->default_value("true"));

    parseResult = options.parse(argc, argv);

    if (parseResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    if (hasOption("testOutput"))
    {
        const std::string& testOutputPath = getAsString("testOutput");
        if (!testOutputPath.ends_with(".png"))
        {
            std::cerr << "--testOutput must be a .png" << std::endl;
            exit(-1);
        }
    }
}

bool hasOption(const std::string& name)
{
    return parseResult.count(name) > 0;
}

bool getAsBool(const std::string& name)
{
    return parseResult[name].as<bool>();
}

int getAsInt(const std::string& name)
{
    return parseResult[name].as<int>();
}

uint32_t getAsUint(const std::string& name)
{
    return parseResult[name].as<uint32_t>();
}

std::string getAsString(const std::string& name)
{
    return parseResult[name].as<std::string>();
}

} // namespace SettingsManager
