#include "test_loader.h"

#define CXXOPTS_NO_EXCEPTIONS
#include <cxxopts.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
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

std::string formatElapsedTime(std::chrono::seconds totalSeconds)
{
    const int hours = static_cast<int>(totalSeconds.count() / 3600);
    const int minutes = static_cast<int>((totalSeconds.count() % 3600) / 60);
    const int seconds = static_cast<int>(totalSeconds.count() % 60);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":" << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

int main(int argc, char** argv)
{
    using namespace cxxopts;

    const auto startTime = std::chrono::steady_clock::now();

    Options options("BiomeinatorRenderingTests", "Rendering tests for Biomeinator");
    OptionAdder optionAdder = options.add_options();

#define ADD_OPTION(name, desc, type, default) optionAdder(name, desc, cxxopts::value<type>()->default_value(default))

    optionAdder("h,help", "Print this message");
    ADD_OPTION("f,filter", "Rendering test filter (regex)", std::string, ".*");
    optionAdder("t,test", "Run exactly one rendering test by name", cxxopts::value<std::string>());

#undef ADD_OPTION

    ParseResult parseResult = options.parse(argc, argv);

    if (parseResult.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    const bool hasExactTest = parseResult.count("test") > 0;
    if (hasExactTest && parseResult.count("filter") > 0)
    {
        std::cerr << "--test and --filter are mutually exclusive" << std::endl;
        return EXIT_FAILURE;
    }

    const std::string exactTestName = hasExactTest ? parseResult["test"].as<std::string>() : "";
    const std::string& testFilterStr = parseResult["filter"].as<std::string>();
    std::regex testFilter;
    if (hasExactTest)
    {
        printf("Running rendering test with exact name: %s\n", exactTestName.c_str());
    }
    else
    {
        printf("Filtering rendering tests with regex: %s\n", testFilterStr.c_str());
        try
        {
            testFilter = std::regex(testFilterStr);
        }
        catch (const std::regex_error& exception)
        {
            std::cerr << "Invalid test filter regex: " << exception.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

    const auto testsOutputPath = std::filesystem::path(CMAKE_BINARY_DIR) / "test_output";
    printf("Tests output path: %s\n", testsOutputPath.generic_string().c_str());
    try
    {
        std::filesystem::create_directories(testsOutputPath);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Filesystem error preparing test output dir '" << testsOutputPath.generic_string()
                  << "': " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<TestCase> tests;
    try
    {
        tests = loadTests(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/tests.json");
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to load tests.json: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    int numTests = 0;
    std::vector<std::string> failedTestNames;
    for (const TestCase& test : tests)
    {
        const bool isSelected = hasExactTest ? test.name == exactTestName : std::regex_search(test.name, testFilter);
        if (!isSelected)
        {
            continue;
        }

        ++numTests;

        int numAsserts = 0;
        int numFailedAsserts = 0;

        printf("\n=============================================\n");
        printf("STARTING TEST: %s\n", test.name.c_str());
        printf("=============================================\n\n");

        const auto generatedImagePath = testsOutputPath / (test.name + "_GENERATED.png");
        const auto goldenCopy = testsOutputPath / (test.name + "_GOLDEN.png");
        const auto diffPath = testsOutputPath / (test.name + "_DIFF.png");
        try
        {
            std::filesystem::remove(generatedImagePath);
            std::filesystem::remove(goldenCopy);
            std::filesystem::remove(diffPath);
            TEST_ASSERT(std::filesystem::is_regular_file(test.goldenPath));
            std::filesystem::copy_file(test.goldenPath, goldenCopy, std::filesystem::copy_options::overwrite_existing);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Filesystem error staging golden for '" << test.name
                      << "' (golden=" << test.goldenPath.generic_string() << "): " << e.what() << std::endl;
            failedTestNames.push_back(test.name);
            continue;
        }

        std::filesystem::path exePath = BIOMEINATOR_EXE_PATH;
        // --renderToFile keeps the window hidden and makes the run headless, which also locks the camera, hides the
        // GUI, and pauses animation so screenshots are deterministic
        std::string command =
            exePath.generic_string() + " --renderToFile=" + generatedImagePath.generic_string();
        for (const std::string& arg : test.args)
        {
            command += " " + arg;
        }
        std::cout << command << std::endl << std::endl;
        const int ret = std::system(command.c_str());
        TEST_ASSERT(ret == 0);
        if (ret != 0)
        {
            std::cerr << "Renderer exited with code " << ret << "; skipping image comparison for '" << test.name
                      << "'. See the renderer diagnostic above.\n";
            failedTestNames.push_back(test.name);
            continue;
        }

        int genW = 0;
        int genH = 0;
        int genC = 0;
        unsigned char* generated = stbi_load(generatedImagePath.generic_string().c_str(), &genW, &genH, &genC, 3);
        TEST_ASSERT(generated != nullptr);

        int goldW = 0;
        int goldH = 0;
        int goldC = 0;
        unsigned char* golden = stbi_load(test.goldenPath.generic_string().c_str(), &goldW, &goldH, &goldC, 3);
        TEST_ASSERT(golden != nullptr);

        const bool widthMatches = (genW == goldW);
        const bool heightMatches = (genH == goldH);

        TEST_ASSERT(widthMatches);
        TEST_ASSERT(heightMatches);

        float rmse = FLT_MAX;
        if (widthMatches && heightMatches)
        {
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

            stbi_write_png(diffPath.generic_string().c_str(), genW, genH, 3, diffImg.data(), genW * 3);

            rmse = std::sqrt(sumSq / count) / 255.f;
        }

        const bool errorUnderThreshold = (rmse <= test.threshold);
        TEST_ASSERT(errorUnderThreshold);

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
        printf("Error:     %.5f\n", rmse);
        printf("Threshold: %.5f\n", test.threshold);
        printf("=============================================\n\n");
    }

    if (numTests == 0)
    {
        std::cerr << "No rendering tests matched the requested selection" << std::endl;
        return EXIT_FAILURE;
    }

    const auto endTime = std::chrono::steady_clock::now();
    const auto elapsedDuration = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime);
    const std::string elapsedTimeStr = formatElapsedTime(elapsedDuration);

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
    printf("Total elapsed time: %s\n", elapsedTimeStr.c_str());
    printf("=============================================\n\n");
    printf("\033[0m");

    return (numFailedTests == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
