// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "terrain/triangle_quads.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace
{

using Triangle = std::array<uint32_t, 3>;

// Rotated to start at its smallest index, which preserves winding
Triangle canonical(Triangle triangle)
{
    std::rotate(triangle.begin(), std::min_element(triangle.begin(), triangle.end()), triangle.end());
    return triangle;
}

std::vector<Triangle> canonicalTriangles(const std::vector<uint32_t>& idxs)
{
    std::vector<Triangle> triangles;
    for (size_t i = 0; i < idxs.size(); i += 3)
    {
        triangles.push_back(canonical({ idxs[i], idxs[i + 1], idxs[i + 2] }));
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

// Checks that the quads' non-degenerate triangles are exactly the input triangles, with winding kept
void checkCoversInput(const std::vector<uint32_t>& triangleIdxs, const TriangleQuads& quads)
{
    REQUIRE(quads.quadIdxs.size() % 4 == 0);
    std::vector<uint32_t> quadTriangleIdxs;
    for (size_t quad = 0; quad < quads.quadIdxs.size() / 4; ++quad)
    {
        const uint32_t* const q = &quads.quadIdxs[quad * 4];
        quadTriangleIdxs.insert(quadTriangleIdxs.end(), { q[0], q[1], q[2] });
        if (quad < quads.numPairedQuads)
        {
            quadTriangleIdxs.insert(quadTriangleIdxs.end(), { q[0], q[2], q[3] });
        }
        else
        {
            CHECK(q[2] == q[3]);
        }
    }
    CHECK(canonicalTriangles(quadTriangleIdxs) == canonicalTriangles(triangleIdxs));
}

} // namespace

TEST_CASE("pairTrianglesIntoQuads pairs a split quad in any rotation", "[triangle_quads]")
{
    const std::vector<uint32_t> idxs = { 2, 3, 0, 1, 2, 0 };
    const TriangleQuads quads = pairTrianglesIntoQuads(idxs);
    CHECK(quads.numPairedQuads == 1);
    CHECK(quads.quadIdxs.size() == 4);
    checkCoversInput(idxs, quads);
}

TEST_CASE("pairTrianglesIntoQuads makes lone triangles degenerate quads after the pairs", "[triangle_quads]")
{
    // A split quad plus a fan triangle that can no longer pair, and a disconnected triangle
    const std::vector<uint32_t> idxs = { 0, 1, 2, 0, 2, 3, 0, 3, 4, 5, 6, 7 };
    const TriangleQuads quads = pairTrianglesIntoQuads(idxs);
    CHECK(quads.numPairedQuads == 1);
    CHECK(quads.quadIdxs.size() == 12);
    checkCoversInput(idxs, quads);
}

TEST_CASE("pairTrianglesIntoQuads does not pair triangles with inconsistent winding", "[triangle_quads]")
{
    // Both triangles run the shared edge 0 -> 2, so a quad could not keep both windings
    const std::vector<uint32_t> idxs = { 0, 2, 1, 0, 2, 3 };
    const TriangleQuads quads = pairTrianglesIntoQuads(idxs);
    CHECK(quads.numPairedQuads == 0);
    checkCoversInput(idxs, quads);
}

TEST_CASE("pairTrianglesIntoQuads pairs every triangle of a closed box", "[triangle_quads]")
{
    // Corners are 0bZYX; each face is split along a diagonal, wound outward
    const std::vector<uint32_t> idxs = {
        0, 2, 3, 0, 3, 1, // -Z
        4, 5, 7, 4, 7, 6, // +Z
        0, 4, 6, 0, 6, 2, // -X
        1, 3, 7, 1, 7, 5, // +X
        0, 1, 5, 0, 5, 4, // -Y
        2, 6, 7, 2, 7, 3, // +Y
    };
    const TriangleQuads quads = pairTrianglesIntoQuads(idxs);
    CHECK(quads.numPairedQuads == 6);
    checkCoversInput(idxs, quads);
}
