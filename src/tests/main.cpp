#include "test_loader.h"

#include <filesystem>
#include <stb/stb_image.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>

static int numAsserts = 0;
static int numFailedAsserts = 0;

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
    const auto testsOutputPath = std::filesystem::absolute("test_output");
    printf("Tests output path: %s\n", testsOutputPath.generic_string().c_str());
    if (std::filesystem::exists(testsOutputPath))
    {
        std::filesystem::remove_all(testsOutputPath);
    }
    std::filesystem::create_directories(testsOutputPath);

    const auto tests = loadTests(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/tests.json");
    for (const TestCase& test : tests)
    {
        printf("\n=============================================\n");
        printf("STARTING TEST: %s\n", test.name.c_str());
        printf("=============================================\n\n");

        const std::filesystem::path goldenCopy = testsOutputPath / (test.name + "_GOLDEN.png");
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
        TEST_ASSERT(rmse <= test.threshold);

        printf("\n=============================================\n");
        printf("FINISHED TEST: %s\n", test.name.c_str());
        printf("Error: %.4f, Threshold: %.4f\n", rmse, test.threshold);
        printf("=============================================\n\n");
    }

    if (numFailedAsserts == 0)
    {
        printf("\033[32mAll (%d) assertions passed.\033[0m\n", numAsserts);
    }
    else
    {
        printf("\033[31m%d/%d assertion(s) failed.\033[0m\n", numFailedAsserts, numAsserts);
    }
    return (numFailedAsserts == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
