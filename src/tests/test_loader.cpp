#include "test_loader.h"

#include <fstream>
#include <iostream>
#include <json.hpp>

#include <stb_image.h>

std::vector<TestCase> loadTests(const std::filesystem::path& jsonPath)
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

    nlohmann::json data;
    try
    {
        data = nlohmann::json::parse(content);
    }
    catch (const nlohmann::json::parse_error& ex)
    {
        std::cerr << "JSON parse error in " << jsonPath.string() << ": " << ex.what() << "\n";
        exit(1);
    }
    catch (const nlohmann::json::exception& ex)
    {
        std::cerr << "JSON error in " << jsonPath.string() << ": " << ex.what() << "\n";
        exit(1);
    }

    const std::filesystem::path testsDir = jsonPath.parent_path();

    std::vector<TestCase> cases;
    for (const nlohmann::json& t : data["tests"])
    {
        const std::string name = t.at("name").get<std::string>();

        TestCase tc;
        tc.name = name;
        tc.goldenPath = testsDir / t.at("golden").get<std::string>();
        tc.threshold = t.value("threshold", 0.0f);
        tc.args = t.value("args", std::vector<std::string>{});

        const std::filesystem::path scene = testsDir / t.at("scene").get<std::string>();
        tc.args.push_back("--scene=" + scene.generic_string());

        cases.push_back(std::move(tc));
    }

    return cases;
}

