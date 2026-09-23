#include "util/packing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <DirectXPackedVector.h>

#include <array>
#include <cstdint>

TEST_CASE("packFloat2ToUint stores each IEEE half in its expected lane", "[unit][packing]")
{
    const uint32_t packed = Util::packFloat2ToUint(1.5f, -2.25f);
    CHECK((packed & 0xFFFFu) == DirectX::PackedVector::XMConvertFloatToHalf(1.5f));
    CHECK((packed >> 16) == DirectX::PackedVector::XMConvertFloatToHalf(-2.25f));
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
    constexpr std::array positions{
        DirectX::XMFLOAT3{ -8.f, -1.f, -8.f },
        DirectX::XMFLOAT3{ 0.f, 42.125f, 15.5f },
        DirectX::XMFLOAT3{ 55.9990234375f, 1022.984375f, 55.9990234375f },
        DirectX::XMFLOAT3{ 1.2345f, 300.333f, -4.5678f },
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
        CHECK(unpacked.pos_OS.x == Catch::Approx(position.x).margin(0.5f / PACKED_TERRAIN_POS_XZ_SCALE));
        CHECK(unpacked.pos_OS.y == Catch::Approx(position.y).margin(0.5f / PACKED_TERRAIN_POS_Y_SCALE));
        CHECK(unpacked.pos_OS.z == Catch::Approx(position.z).margin(0.5f / PACKED_TERRAIN_POS_XZ_SCALE));
        CHECK(unpacked.uv.x == Catch::Approx(source.uv.x).margin(0.5f / 255.f));
        CHECK(unpacked.uv.y == Catch::Approx(source.uv.y).margin(0.5f / 255.f));
        CHECK(unpacked.packedNor == source.packedNor);
    }
}
