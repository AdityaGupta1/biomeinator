#include "terrain/block.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace
{

constexpr BlockFace opposite(BlockFace face)
{
    switch (face)
    {
        case BlockFace::X_POS:
            return BlockFace::X_NEG;
        case BlockFace::Z_POS:
            return BlockFace::Z_NEG;
        case BlockFace::X_NEG:
            return BlockFace::X_POS;
        case BlockFace::Z_NEG:
            return BlockFace::Z_POS;
        case BlockFace::Y_POS:
            return BlockFace::Y_NEG;
        case BlockFace::Y_NEG:
            return BlockFace::Y_POS;
        default:
            return BlockFace::COUNT;
    }
}

bool sameVector(const glm::vec3& left, const glm::vec3& right)
{
    return glm::all(glm::equal(left, right));
}

} // namespace

TEST_CASE("block face bases are orthogonal and orient local axes", "[unit][block_rules]")
{
    for (uint8_t faceIdx = 0; faceIdx < blockFaceCount; ++faceIdx)
    {
        const BlockFace face = static_cast<BlockFace>(faceIdx);
        const BlockFaceBasis& basis = blockFaceBasis(face);
        const glm::vec3 normal = basis.normal;
        const glm::vec3 tangentX = basis.tangentX;
        const glm::vec3 tangentZ = basis.tangentZ;

        CAPTURE(faceIdx);
        CHECK(glm::dot(normal, tangentX) == 0.f);
        CHECK(glm::dot(normal, tangentZ) == 0.f);
        CHECK(glm::dot(tangentX, tangentZ) == 0.f);
        CHECK(glm::dot(normal, normal) == 1.f);
        CHECK(glm::dot(tangentX, tangentX) == 1.f);
        CHECK(glm::dot(tangentZ, tangentZ) == 1.f);

        CHECK(sameVector(orientToBlockFace({ 1.f, 0.f, 0.f }, face), glm::vec3(basis.tangentX)));
        CHECK(sameVector(orientToBlockFace({ 0.f, 1.f, 0.f }, face), glm::vec3(basis.normal)));
        CHECK(sameVector(orientToBlockFace({ 0.f, 0.f, 1.f }, face), glm::vec3(basis.tangentZ)));
    }
}

TEST_CASE("air neighbors expose faces", "[unit][block_rules]")
{
    constexpr std::array types{
        BlockType::SOLID,
        BlockType::TRANSPARENT_CUTOUT,
        BlockType::GLASS,
        BlockType::WATER,
    };

    for (const BlockType type : types)
    {
        for (int faceIdx = 0; faceIdx < blockFaceCount; ++faceIdx)
        {
            CAPTURE(static_cast<int>(type), faceIdx);
            CHECK(blockFaceVisible(type, BlockShape::CUBE, BlockType::AIR, BlockShape::CUBE, faceIdx));
        }
    }
}

TEST_CASE("solid cubes cull shared solid faces", "[unit][block_rules]")
{
    for (int faceIdx = 0; faceIdx < blockFaceCount; ++faceIdx)
    {
        CAPTURE(faceIdx);
        CHECK_FALSE(blockFaceVisible(BlockType::SOLID, BlockShape::CUBE, BlockType::SOLID, BlockShape::CUBE, faceIdx));
    }
}

TEST_CASE("decorator-shaped neighbors never hide opaque cube faces", "[unit][block_rules]")
{
    constexpr std::array opaqueTypes{
        BlockType::SOLID,
        BlockType::TRANSPARENT_CUTOUT,
        BlockType::GLASS,
    };
    constexpr std::array decoratorShapes{
        BlockShape::X_SHAPED,
        BlockShape::DECORATOR_CUSTOM,
    };

    for (const BlockType type : opaqueTypes)
    {
        for (const BlockShape neighborShape : decoratorShapes)
        {
            CAPTURE(static_cast<int>(type), static_cast<int>(neighborShape));
            CHECK(blockFaceVisible(type, BlockShape::CUBE, BlockType::SOLID, neighborShape, 0));
        }
    }
}

TEST_CASE("shared cutout boundaries have exactly one owner", "[unit][block_rules]")
{
    constexpr std::array positiveFaces{ BlockFace::X_POS, BlockFace::Z_POS, BlockFace::Y_POS };

    for (const BlockFace face : positiveFaces)
    {
        const bool positiveOwner = blockFaceVisible(BlockType::TRANSPARENT_CUTOUT,
                                                    BlockShape::CUBE,
                                                    BlockType::TRANSPARENT_CUTOUT,
                                                    BlockShape::CUBE,
                                                    blockFaceIndex(face));
        const bool negativeOwner = blockFaceVisible(BlockType::TRANSPARENT_CUTOUT,
                                                    BlockShape::CUBE,
                                                    BlockType::TRANSPARENT_CUTOUT,
                                                    BlockShape::CUBE,
                                                    blockFaceIndex(opposite(face)));
        CAPTURE(static_cast<int>(face));
        CHECK(positiveOwner);
        CHECK_FALSE(negativeOwner);
        CHECK(positiveOwner != negativeOwner);
    }
}

TEST_CASE("glass culls internal and rock-adjacent faces", "[unit][block_rules]")
{
    CHECK_FALSE(blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::GLASS, BlockShape::CUBE, 0));
    CHECK_FALSE(blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::SOLID, BlockShape::CUBE, 0));
    CHECK(blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::TRANSPARENT_CUTOUT, BlockShape::CUBE, 0));
    CHECK(blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::WATER, BlockShape::CUBE, 0));
}

TEST_CASE("liquid top only forces its upward surface against non-air", "[unit][block_rules]")
{
    for (int faceIdx = 0; faceIdx < blockFaceCount; ++faceIdx)
    {
        const bool visible =
            blockFaceVisible(BlockType::WATER, BlockShape::LIQUID_TOP, BlockType::WATER, BlockShape::CUBE, faceIdx);
        CAPTURE(faceIdx);
        CHECK(visible == (faceIdx == blockFaceIndex(BlockFace::Y_POS)));
    }
}

TEST_CASE("solid liquid-top geometry exposes the expected stepped boundaries", "[unit][block_rules]")
{
    CHECK(blockFaceVisible(BlockType::SOLID,
                           BlockShape::LIQUID_TOP,
                           BlockType::SOLID,
                           BlockShape::CUBE,
                           blockFaceIndex(BlockFace::Y_POS)));
    CHECK(blockFaceVisible(BlockType::SOLID,
                           BlockShape::CUBE,
                           BlockType::SOLID,
                           BlockShape::LIQUID_TOP,
                           blockFaceIndex(BlockFace::Y_NEG)));
    CHECK(blockFaceVisible(BlockType::SOLID,
                           BlockShape::CUBE,
                           BlockType::SOLID,
                           BlockShape::LIQUID_TOP,
                           blockFaceIndex(BlockFace::X_POS)));
    CHECK_FALSE(blockFaceVisible(BlockType::SOLID,
                                 BlockShape::LIQUID_TOP,
                                 BlockType::SOLID,
                                 BlockShape::LIQUID_TOP,
                                 blockFaceIndex(BlockFace::X_POS)));
}
