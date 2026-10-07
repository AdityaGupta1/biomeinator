// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#pragma once

#include <cstdint>
#include <vector>

// Triangles regrouped into the quad topology of terrain faces, whose two triangles are
// (0, 1, 2) and (0, 2, 3) of four vertices
struct TriangleQuads
{
    // Four vertex indices per quad. Pairs come first; each remaining lone triangle repeats its last
    // vertex, which makes the quad's second triangle degenerate.
    std::vector<uint32_t> quadIdxs;
    uint32_t numPairedQuads{ 0 };
};

// Pairs triangles that share an edge with opposite winding, so both keep their winding
TriangleQuads pairTrianglesIntoQuads(const std::vector<uint32_t>& triangleIdxs);
