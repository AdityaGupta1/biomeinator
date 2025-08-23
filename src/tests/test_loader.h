#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct TestCase
{
    std::string name;
    std::vector<std::string> args;
    std::filesystem::path goldenPath;
    float threshold;
};

std::vector<TestCase> loadTests(const std::filesystem::path& jsonPath);

