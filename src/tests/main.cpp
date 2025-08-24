#include "test_loader.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts/cxxopts.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <filesystem>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <regex>
#include <vector>

#define TEST_ASSERT(cond)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        ++numAsserts;                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++numFailedAsserts;                                                                                        \
            std::cerr << "\033[31mASSERTION FAILED: " #cond "\n"                                                       \
                      << "  File: " << __FILE__ << ":" << __LINE__ << "\033[0m\n";                                     \
        }                                                                                                              \
    } while (0)

int main(int argc, char** argv)
{
    using namespace cxxopts;

    Options options("BiomeinatorTests", "Tests for Biomeinator");
    OptionAdder optionAdder = options.add_options();
    optionAdder("h,help", "Print this message");
    optionAdder("f,filter", "Test filter (regex)", cxxopts::value<std::string>()->default_value(".*"));

    ParseResult parseResult = options.parse(argc, argv);

    if (parseResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    const std::string& testFilterStr = parseResult["filter"].as<std::string>();
    printf("Filtering tests with regex: %s\n", testFilterStr.c_str());
    const std::regex testFilter(parseResult["filter"].as<std::string>());

    const auto testsOutputPath = std::filesystem::absolute("test_output");
    printf("Tests output path: %s\n", testsOutputPath.generic_string().c_str());
    if (std::filesystem::exists(testsOutputPath))
    {
        std::filesystem::remove_all(testsOutputPath);
    }
    std::filesystem::create_directories(testsOutputPath);

    const auto tests = loadTests(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/tests.json");
    int numTests = 0;
    std::vector<std::string> failedTestNames;
    for (const TestCase& test : tests)
    {
        if (!std::regex_search(test.name, testFilter))
        {
            continue;
        }

        ++numTests;

        int numAsserts = 0;
        int numFailedAsserts = 0;

        printf("\n=============================================\n");
        printf("STARTING TEST: %s\n", test.name.c_str());
        printf("=============================================\n\n");

        const std::filesystem::path goldenCopy = testsOutputPath / (test.name + "_GOLDEN.png");
        TEST_ASSERT(std::filesystem::is_regular_file(test.goldenPath));
        std::filesystem::copy_file(test.goldenPath, goldenCopy, std::filesystem::copy_options::overwrite_existing);

        std::filesystem::path exePath = BIOMEINATOR_EXE_PATH;
        const auto generatedImagePath = testsOutputPath / (test.name + "_GENERATED.png");
        std::string command = (exePath.generic_string() + " --testOutput=" + generatedImagePath.generic_string());
        for (const std::string& arg : test.args)
        {
            command += " " + arg;
        }
        std::cout << command << std::endl << std::endl;
        const int ret = std::system(command.c_str());
        TEST_ASSERT(ret == 0);

        int genW = 0;
        int genH = 0;
        int genC = 0;
        unsigned char* generated = stbi_load(generatedImagePath.generic_string().c_str(), &genW, &genH, &genC, 3);
        TEST_ASSERT(generated != nullptr);

        int goldW = 0;
        int goldH = 0;
        int goldC = 0;
        unsigned char* golden = stbi_load(goldenCopy.generic_string().c_str(), &goldW, &goldH, &goldC, 3);
        TEST_ASSERT(golden != nullptr);

        TEST_ASSERT(genW == goldW);
        TEST_ASSERT(genH == goldH);

        float sumSq = 0.f;
        const size_t count = static_cast<size_t>(genW) * genH * 3;
        std::vector<uint8_t> diffImg(count);
        for (size_t i = 0; i < count; ++i)
        {
            const int diff = static_cast<int>(generated[i]) - static_cast<int>(golden[i]);
            sumSq += static_cast<float>(diff * diff);
            diffImg[i] = static_cast<uint8_t>(std::clamp(std::abs(diff), 0, 255));
        }
        stbi_image_free(generated);
        stbi_image_free(golden);

        const auto diffPath = testsOutputPath / (test.name + "_DIFF.png");
        const int writeResult =
            stbi_write_png(diffPath.generic_string().c_str(), genW, genH, 3, diffImg.data(), genW * 3);
        TEST_ASSERT(writeResult != 0);

        const float rmse = std::sqrt(sumSq / count) / 255.f;
        TEST_ASSERT(rmse <= test.threshold);

        if (numFailedAsserts == 0)
        {
            printf("\033[32mAll (%d) assertion(s) passed.\033[0m\n", numAsserts);
        }
        else
        {
            printf("\033[31m%d/%d assertion(s) failed.\033[0m\n", numFailedAsserts, numAsserts);
            failedTestNames.push_back(test.name);
        }

        printf("\n=============================================\n");
        printf("FINISHED TEST: %s\n", test.name.c_str());
        printf("Error: %.4f, Threshold: %.4f\n", rmse, test.threshold);
        printf("=============================================\n\n");
    }

    const int numFailedTests = failedTestNames.size();
    const bool didFail = (numFailedTests > 0);
    printf(didFail ? "\033[31m" : "\033[32m");
    printf("\n=============================================\n");
    if (numFailedTests == 0)
    {
        printf("All (%d) test(s) passed.\n", numTests);
    }
    else
    {
        printf("%d/%d tests(s) failed:\n", numFailedTests, numTests);
        for (const auto& testName : failedTestNames)
        {
            printf("- %s\n", testName.c_str());
        }
    }
    printf("=============================================\n\n");
    printf("\033[0m");

    return (numFailedTests == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
