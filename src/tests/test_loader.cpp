#include "test_loader.h"

#include <fstream>
#include <tinygltf/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

std::vector<TestCase> LoadTests(const std::filesystem::path& jsonPath)
{
    std::ifstream file(jsonPath);
    if (!file.is_open())
    {
        return {};
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content.empty())
    {
        return {};
    }

    if (content.front() != '{')
    {
        content.insert(content.begin(), '{');
        content.push_back('}');
    }

    for (std::string::size_type pos = 0; (pos = content.find(",}")) != std::string::npos; )
    {
        content.erase(pos, 1);
    }
    for (std::string::size_type pos = 0; (pos = content.find(",]")) != std::string::npos; )
    {
        content.erase(pos, 1);
    }

    const nlohmann::json data = nlohmann::json::parse(content);
    const std::filesystem::path testsDir = jsonPath.parent_path();

    std::vector<TestCase> cases;
    for (const nlohmann::json& t : data["tests"])
    {
        const std::string name = t.at("name").get<std::string>();

        TestCase tc;
        tc.name = name;
        tc.golden = testsDir / t.at("golden").get<std::string>();
        tc.output = std::filesystem::path("test_output") / (name + "_GENERATED.png");
        tc.threshold = t.value("threshold", 0.0f);
        tc.args = t.value("args", std::vector<std::string>{});

        const std::filesystem::path scene = testsDir / t.at("scene").get<std::string>();
        tc.args.push_back("--scene=" + scene.string());

        int width = 0;
        int height = 0;
        int comp = 0;
        if (stbi_info(tc.golden.string().c_str(), &width, &height, &comp) != 0)
        {
            tc.args.push_back("--width=" + std::to_string(width));
            tc.args.push_back("--height=" + std::to_string(height));
        }

        cases.push_back(std::move(tc));
    }

    return cases;
}

