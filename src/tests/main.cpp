#include "test_loader.h"

#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <stb/stb_image.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>

TEST_CASE("Image-based tests")
{
    const auto testsOutputPath = std::filesystem::absolute("test_output");
    static std::once_flag once;
    std::call_once(once, [&]{
        printf("Tests output path: %s\n", testsOutputPath.generic_string().c_str());
        if (std::filesystem::exists(testsOutputPath))
        {
            std::filesystem::remove_all(testsOutputPath);
        }
        std::filesystem::create_directories(testsOutputPath);
    });

    const auto tests = loadTests(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/tests.json");
    for (const TestCase& test : tests)
    {
        DYNAMIC_SECTION(test.name)
        {
            printf("\n=============================================\n");
            printf("STARTING TEST: %s\n", test.name.c_str());
            printf("=============================================\n\n");

            const std::filesystem::path goldenCopy = testsOutputPath / (test.name + "_GOLDEN.png");
            std::filesystem::copy_file(test.goldenPath, goldenCopy, std::filesystem::copy_options::overwrite_existing);

            std::filesystem::path exePath = BIOMEINATOR_EXE_PATH;
            const auto generatedImagePath = testsOutputPath / (test.name + "_GENERATED.png");
            std::string command = (exePath.string() + " --test-output " + generatedImagePath.string());
            for (const std::string& arg : test.args)
            {
                command += " " + arg;
            }
            std::cout << command << std::endl << std::endl;
            const int ret = std::system(command.c_str());
            REQUIRE(ret == 0);

            int genW = 0;
            int genH = 0;
            int genC = 0;
            unsigned char* generated = stbi_load(generatedImagePath.string().c_str(), &genW, &genH, &genC, 3);
            REQUIRE(generated != nullptr);

            int goldW = 0;
            int goldH = 0;
            int goldC = 0;
            unsigned char* golden = stbi_load(goldenCopy.string().c_str(), &goldW, &goldH, &goldC, 3);
            REQUIRE(golden != nullptr);

            REQUIRE(genW == goldW);
            REQUIRE(genH == goldH);

            double sumSq = 0.0;
            const size_t count = static_cast<size_t>(genW) * genH * 3;
            for (size_t i = 0; i < count; ++i)
            {
                const double diff = static_cast<double>(generated[i]) - static_cast<double>(golden[i]);
                sumSq += diff * diff;
            }
            stbi_image_free(generated);
            stbi_image_free(golden);

            const double rmse = std::sqrt(sumSq / count) / 255.0;
            REQUIRE(rmse <= test.threshold);

            printf("\n=============================================\n");
            printf("FINISHED TEST: %s\n", test.name.c_str());
            printf("Error: %.4f, Threshold: %.4f\n", rmse, test.threshold);
            printf("=============================================\n\n");
        }
    }
}
