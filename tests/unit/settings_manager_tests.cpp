#include "settings_manager.h"

#include "rendering/common/common_enums.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{

SettingsManager::ParseArgsOutcome parse(std::vector<std::string> args)
{
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args)
    {
        argv.push_back(arg.c_str());
    }
    return SettingsManager::tryParseArgs(static_cast<int>(argv.size()), argv.data());
}

void requireSuccess(std::vector<std::string> args)
{
    const SettingsManager::ParseArgsOutcome outcome = parse(std::move(args));
    INFO(outcome.message);
    REQUIRE(outcome.status == SettingsManager::ParseArgsStatus::Success);
}

} // namespace

TEST_CASE("SettingsManager installs documented defaults", "[unit][settings_manager]")
{
    requireSuccess({ "Biomeinator" });

    CHECK(SettingsManager::getAsUint("width") == 1920);
    CHECK(SettingsManager::getAsUint("height") == 1080);
    CHECK(SettingsManager::getAsInt("renderDistance") == 30);
    CHECK(SettingsManager::getAsFloat("movementSpeed") == Catch::Approx(12.f));
    CHECK(SettingsManager::getAsString("debugView") == "off");
    CHECK_FALSE(SettingsManager::getAsBool("voxelMode"));
    CHECK(SettingsManager::getAsUint("antialiasingMode") == static_cast<uint32_t>(AntialiasingMode::NONE));
    CHECK(SettingsManager::getWorldSeed() == 1738);
    CHECK_FALSE(SettingsManager::isTestMode());
    CHECK_FALSE(SettingsManager::isPerfMode());
    CHECK_FALSE(SettingsManager::isHeadless());
}

TEST_CASE("SettingsManager parses overrides and keeps the cached world seed synchronized", "[unit][settings_manager]")
{
    requireSuccess({ "Biomeinator",
                     "--width=1280",
                     "--renderDistance=18",
                     "--movementSpeed=20.5",
                     "--debugView=normals",
                     "--worldSeed=99",
                     "--voxelMode=true",
                     "--antialiasingMode=1" });

    CHECK(SettingsManager::getAsUint("width") == 1280);
    CHECK(SettingsManager::getAsInt("renderDistance") == 18);
    CHECK(SettingsManager::getAsFloat("movementSpeed") == Catch::Approx(20.5f));
    CHECK(SettingsManager::getAsString("debugView") == "normals");
    CHECK(SettingsManager::getAsBool("voxelMode"));
    CHECK(SettingsManager::getAsUint("antialiasingMode") == static_cast<uint32_t>(AntialiasingMode::ACCUMULATE));
    CHECK(SettingsManager::getAsUint("worldSeed") == 99);
    CHECK(SettingsManager::getWorldSeed() == 99);

    SettingsManager::setAsBool("showGui", false);
    CHECK_FALSE(SettingsManager::getAsBool("showGui"));
    SettingsManager::toggleBool("showGui");
    CHECK(SettingsManager::getAsBool("showGui"));
    SettingsManager::setAsInt("renderDistance", 24);
    SettingsManager::setAsUint("width", 1600);
    SettingsManager::setAsFloat("movementSpeed", 7.25f);
    SettingsManager::setAsString("debugView", "off");
    SettingsManager::setWorldSeed(1234);
    CHECK(SettingsManager::getAsInt("renderDistance") == 24);
    CHECK(SettingsManager::getAsUint("width") == 1600);
    CHECK(SettingsManager::getAsFloat("movementSpeed") == Catch::Approx(7.25f));
    CHECK(SettingsManager::getAsString("debugView") == "off");
    CHECK(SettingsManager::getAsUint("worldSeed") == 1234);
    CHECK(SettingsManager::getWorldSeed() == 1234);

    bool foundWidth = false;
    SettingsManager::forEachSetting(
        [&foundWidth](const std::string& name, const SettingsManager::SettingValue& value)
        {
            if (name == "width")
            {
                foundWidth = std::get<uint32_t>(value) == 1600;
            }
        });
    CHECK(foundWidth);
}

TEST_CASE("SettingsManager applies mode-dependent defaults without overriding explicit values",
          "[unit][settings_manager]")
{
    SECTION("voxel mode defaults to DLSS")
    {
        requireSuccess({ "Biomeinator", "--voxelMode=true" });
        CHECK(SettingsManager::getAsUint("antialiasingMode") == static_cast<uint32_t>(AntialiasingMode::DLSS));
    }

    SECTION("world import enables voxel mode and its default antialiasing")
    {
        requireSuccess({ "Biomeinator", "--world=world.json" });
        CHECK(SettingsManager::getAsBool("voxelMode"));
        CHECK(SettingsManager::getAsUint("antialiasingMode") == static_cast<uint32_t>(AntialiasingMode::DLSS));
    }

    SECTION("explicit antialiasing wins")
    {
        requireSuccess({ "Biomeinator", "--voxelMode=true", "--antialiasingMode=0" });
        CHECK(SettingsManager::getAsUint("antialiasingMode") == static_cast<uint32_t>(AntialiasingMode::NONE));
    }
}

TEST_CASE("SettingsManager applies and permits overriding headless defaults", "[unit][settings_manager]")
{
    SECTION("test runs receive deterministic defaults")
    {
        requireSuccess({ "Biomeinator", "--testOutput=result.png" });
        CHECK(SettingsManager::isTestMode());
        CHECK_FALSE(SettingsManager::isPerfMode());
        CHECK(SettingsManager::isHeadless());
        CHECK_FALSE(SettingsManager::getAsBool("sharc"));
        CHECK(SettingsManager::getAsBool("lockCamera"));
        CHECK_FALSE(SettingsManager::getAsBool("showGui"));
        CHECK(SettingsManager::getAsBool("animTimePaused"));
        CHECK_FALSE(SettingsManager::getAsBool("useVsync"));
    }

    SECTION("explicit values win over headless defaults")
    {
        requireSuccess({ "Biomeinator",
                         "--perfOutput=result.json",
                         "--sharc=true",
                         "--lockCamera=false",
                         "--showGui=true",
                         "--animTimePaused=false",
                         "--useVsync=true" });
        CHECK_FALSE(SettingsManager::isTestMode());
        CHECK(SettingsManager::isPerfMode());
        CHECK(SettingsManager::isHeadless());
        CHECK(SettingsManager::getAsBool("sharc"));
        CHECK_FALSE(SettingsManager::getAsBool("lockCamera"));
        CHECK(SettingsManager::getAsBool("showGui"));
        CHECK_FALSE(SettingsManager::getAsBool("animTimePaused"));
        CHECK(SettingsManager::getAsBool("useVsync"));
    }
}

TEST_CASE("SettingsManager reports help and invalid command lines without exiting", "[unit][settings_manager]")
{
    const SettingsManager::ParseArgsOutcome help = parse({ "Biomeinator", "--help" });
    CHECK(help.status == SettingsManager::ParseArgsStatus::Help);
    CHECK(help.message.find("Real-time path traced voxel engine") != std::string::npos);

    const std::vector<std::pair<std::vector<std::string>, std::string>> invalidCases{
        { { "Biomeinator", "--testOutput=result.jpg" }, "--testOutput must be a .png" },
        { { "Biomeinator", "--perfOutput=result.txt" }, "--perfOutput must be a .json" },
        { { "Biomeinator", "--testOutput=result.png", "--perfOutput=result.json" }, "mutually exclusive" },
        { { "Biomeinator", "--samplingMode=3" }, "samplingMode" },
        { { "Biomeinator", "--antialiasingMode=3" }, "antialiasingMode" },
        { { "Biomeinator", "--tonemapping=4" }, "tonemapping" },
        { { "Biomeinator", "--dlssPreset=2" }, "dlssPreset" },
        { { "Biomeinator", "--sharcCapacityLog2=15" }, "SHARC" },
        { { "Biomeinator", "--sharcDownscale=0" }, "SHARC" },
        { { "Biomeinator", "--sharcSceneScale=0" }, "SHARC" },
        { { "Biomeinator", "--sharcRoughnessMin=1.1" }, "SHARC" },
        { { "Biomeinator", "--sharcDebug=5" }, "SHARC" },
        { { "Biomeinator", "--sharcAccumulationFrames=1025" }, "SHARC" },
        { { "Biomeinator", "--sharcStaleFrames=7" }, "SHARC" },
        { { "Biomeinator", "--width=wide" }, "failed to parse" },
        { { "Biomeinator", "--voxelMode=maybe" }, "failed to parse" },
        { { "Biomeinator", "--notASetting=1" }, "notASetting" },
    };

    for (const auto& [args, expectedMessage] : invalidCases)
    {
        const SettingsManager::ParseArgsOutcome outcome = parse(args);
        CAPTURE(args, outcome.message);
        CHECK(outcome.status == SettingsManager::ParseArgsStatus::Error);
        CHECK(outcome.message.find(expectedMessage) != std::string::npos);
    }
}

TEST_CASE("SettingsManager leaves live settings unchanged after a failed parse", "[unit][settings_manager]")
{
    requireSuccess({ "Biomeinator", "--width=1280", "--worldSeed=99" });

    const SettingsManager::ParseArgsOutcome outcome =
        parse({ "Biomeinator", "--width=640", "--worldSeed=1234", "--samplingMode=3" });
    REQUIRE(outcome.status == SettingsManager::ParseArgsStatus::Error);
    CHECK(SettingsManager::getAsUint("width") == 1280);
    CHECK(SettingsManager::getAsUint("worldSeed") == 99);
    CHECK(SettingsManager::getWorldSeed() == 99);
}
