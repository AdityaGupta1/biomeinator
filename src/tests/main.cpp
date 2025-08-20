#include "test_loader.h"

#include <catch2/catch_all.hpp>
#include <filesystem>
#include <stb/stb_image.h>
#include <cmath>
#include <cstdlib>

TEST_CASE("Render regression tests")
{
    const auto tests = LoadTests(std::filesystem::path(CMAKE_SOURCE_DIR) / "tests/tests.json");
    for (const TestCase &test : tests)
    {
        DYNAMIC_SECTION(test.name)
        {
            std::filesystem::create_directories(test.output.parent_path());
            const std::filesystem::path goldenCopy = test.output.parent_path() / (test.name + "_GOLDEN.png");
            std::filesystem::copy_file(test.golden, goldenCopy, std::filesystem::copy_options::overwrite_existing);

            std::string command = std::string("./Biomeinator.exe --test-output ") + test.output.string();
            for (const std::string &arg : test.args)
            {
                command += " " + arg;
            }
            const int ret = std::system(command.c_str());
            REQUIRE(ret == 0);

            int genW = 0;
            int genH = 0;
            int genC = 0;
            unsigned char *generated = stbi_load(test.output.string().c_str(), &genW, &genH, &genC, 3);
            REQUIRE(generated != nullptr);

            int goldW = 0;
            int goldH = 0;
            int goldC = 0;
            unsigned char *golden = stbi_load(goldenCopy.string().c_str(), &goldW, &goldH, &goldC, 3);
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
        }
    }
}

