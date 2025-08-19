#include "settings_manager.h"

#define CXXOPTS_NO_EXCEPTIONS
#include "cxxopts/cxxopts.hpp"

namespace SettingsManager
{

using namespace cxxopts;

ParseResult parseResult;

void parseArgs(const int argc, const char* const* argv)
{
    Options options("Biomeinator.exe", "Real-time path traced voxel engine");
    OptionAdder optionAdder = options.add_options();
    optionAdder("h,help", "Print usage");
    optionAdder("width", "Window width", cxxopts::value<uint32_t>()->default_value("1920"));
    optionAdder("height", "Window height", cxxopts::value<uint32_t>()->default_value("1080"));
    optionAdder("spp", "Samples per pixel", cxxopts::value<uint32_t>()->default_value("16"));
    optionAdder("maxPathDepth", "Maximum path depth", cxxopts::value<uint32_t>()->default_value("12"));
    optionAdder("scene", "Scene file (*.gltf; *.glb)", cxxopts::value<std::string>());
    optionAdder("test-output", "Render one frame and save screenshot", cxxopts::value<std::string>());

    parseResult = options.parse(argc, argv);

    if (parseResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
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
