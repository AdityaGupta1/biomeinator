#include "util/packing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <random>

TEST_CASE("packFloat2ToUint stores each IEEE half in its expected lane", "[unit][packing]")
{
    const uint32_t packed = Util::packFloat2ToUint(1.5f, -2.25f);
    CHECK(packed == 0xC0803E00u); // IEEE binary16: 1.5 = 0x3E00, -2.25 = 0xC080.
}

TEST_CASE("packSnorm2ToUint clamps and packs signed-normalized endpoints", "[unit][packing]")
{
    CHECK(Util::packSnorm2ToUint(-1.f, 1.f) == 0x7FFF8001u);
    CHECK(Util::packSnorm2ToUint(-2.f, 2.f) == 0x7FFF8001u);
    CHECK(Util::packSnorm2ToUint(0.f, 0.f) == 0u);
}

TEST_CASE("octEncode maps canonical normals to stable encodings", "[unit][packing]")
{
    CHECK(Util::octEncode({ 0.f, 0.f, 1.f }) == 0u);
    CHECK(Util::octEncode({ 1.f, 0.f, 0.f }) == 0x00007FFFu);
    CHECK(Util::octEncode({ -1.f, 0.f, 0.f }) == 0x00008001u);
    CHECK(Util::octEncode({ 0.f, 0.f, -1.f }) == 0x7FFF7FFFu);
}

TEST_CASE("packUnorm8 clamps and rounds to the nearest byte", "[unit][packing]")
{
    CHECK(Util::packUnorm8(-1.f) == 0u);
    CHECK(Util::packUnorm8(0.f) == 0u);
    CHECK(Util::packUnorm8(0.5f) == 128u);
    CHECK(Util::packUnorm8(1.f) == 255u);
    CHECK(Util::packUnorm8(2.f) == 255u);
}

TEST_CASE("terrain vertex packing round-trips within quantization error", "[unit][packing]")
{
    // Literal words pin the HLSL wire layout independently of the CPU decoder.
    Vertex wireSource{};
    wireSource.pos_OS = { -7.5f, 42.125f, 15.5f };
    wireSource.uv = { 51.f / 255.f, 204.f / 255.f };
    wireSource.packedNor = 0xDEADBEEFu;
    const PackedTerrainVertex encoded = Util::packTerrainVertex(wireSource);
    CHECK(encoded.packedPosXY == 0x0AC80200u);
    CHECK(encoded.packedPosZUv == 0xCC335E00u);
    CHECK(encoded.packedNor == 0xDEADBEEFu);

    // Decode independently authored data, not a value returned by the encoder.
    const Vertex decoded = Util::unpackTerrainVertex({ 0x00010002u, 0xABCD1234u, 0x12345678u });
    CHECK(decoded.pos_OS.x == -7.998046875f);
    CHECK(decoded.pos_OS.y == -0.984375f);
    CHECK(decoded.pos_OS.z == -3.44921875f);
    CHECK(decoded.uv.x == 205.f / 255.f);
    CHECK(decoded.uv.y == 171.f / 255.f);
    CHECK(decoded.packedNor == 0x12345678u);

    constexpr std::array positions{
        DirectX::XMFLOAT3{ -8.f, -1.f, -8.f },
        DirectX::XMFLOAT3{ 0.f, 42.125f, 15.5f },
        DirectX::XMFLOAT3{ 55.9990234375f, 1022.984375f, 55.9990234375f },
        DirectX::XMFLOAT3{ 1.2345f, 300.333f, -4.5678f },
        DirectX::XMFLOAT3{ 50.00054931640625f, 1000.010009765625f, 50.00054931640625f },
    };

    for (const DirectX::XMFLOAT3 position : positions)
    {
        Vertex source{};
        source.pos_OS = position;
        source.packedNor = 0xDEADBEEFu;
        source.uv = { 0.123f, 0.987f };

        const PackedTerrainVertex packed = Util::packTerrainVertex(source);
        const Vertex unpacked = Util::unpackTerrainVertex(packed);

        CAPTURE(position.x, position.y, position.z);
        CHECK(unpacked.pos_OS.x == Catch::Approx(position.x).epsilon(0).margin(0.5 / 1024));
        CHECK(unpacked.pos_OS.y == Catch::Approx(position.y).epsilon(0).margin(0.5 / 64));
        CHECK(unpacked.pos_OS.z == Catch::Approx(position.z).epsilon(0).margin(0.5 / 1024));
        // The non-power-of-two UV decode has at most one float rounding error in addition.
        CHECK(unpacked.uv.x == Catch::Approx(source.uv.x).epsilon(0).margin(0.5 / 255 + 1e-7));
        CHECK(unpacked.uv.y == Catch::Approx(source.uv.y).epsilon(0).margin(0.5 / 255 + 1e-7));
        CHECK(unpacked.packedNor == source.packedNor);
    }
}

TEST_CASE("terrain vertex packing preserves randomized in-range values", "[unit][packing][stress]")
{
    constexpr uint32_t seed = 0xBACC1234u;
    constexpr float maxXZ = 55.9990234375f;
    constexpr float maxY = 1022.984375f;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> xzDistribution(-8.f, maxXZ);
    std::uniform_real_distribution<float> yDistribution(-1.f, maxY);
    std::uniform_real_distribution<float> uvDistribution(0.f, 1.f);
    INFO("seed=" << seed);

    for (int sample = 0; sample < 10000; ++sample)
    {
        Vertex source{};
        source.pos_OS = { xzDistribution(rng), yDistribution(rng), xzDistribution(rng) };
        source.packedNor = rng();
        source.uv = { uvDistribution(rng), uvDistribution(rng) };

        const Vertex unpacked = Util::unpackTerrainVertex(Util::packTerrainVertex(source));

        CAPTURE(sample);
        CHECK(unpacked.pos_OS.x == Catch::Approx(source.pos_OS.x).epsilon(0).margin(0.5 / 1024));
        CHECK(unpacked.pos_OS.y == Catch::Approx(source.pos_OS.y).epsilon(0).margin(0.5 / 64));
        CHECK(unpacked.pos_OS.z == Catch::Approx(source.pos_OS.z).epsilon(0).margin(0.5 / 1024));
        CHECK(unpacked.uv.x == Catch::Approx(source.uv.x).epsilon(0).margin(0.5 / 255 + 1e-7));
        CHECK(unpacked.uv.y == Catch::Approx(source.uv.y).epsilon(0).margin(0.5 / 255 + 1e-7));
        CHECK(unpacked.packedNor == source.packedNor);
    }
}
