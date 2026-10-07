// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta

#include "triangle_quads.h"

#include "debug.h"

#include <unordered_map>

TriangleQuads pairTrianglesIntoQuads(const std::vector<uint32_t>& triangleIdxs)
{
    ASSERT(triangleIdxs.size() % 3 == 0);
    const auto numTriangles = static_cast<uint32_t>(triangleIdxs.size() / 3);

    const auto vertIdx = [&](const uint32_t triangle, const uint32_t corner)
    {
        return triangleIdxs[triangle * 3 + corner % 3];
    };
    const auto edgeKey = [](const uint32_t from, const uint32_t to)
    {
        return (static_cast<uint64_t>(from) << 32) | to;
    };

    // Triangles in ascending order per directed edge
    std::unordered_map<uint64_t, std::vector<uint32_t>> trianglesByEdge;
    for (uint32_t triangle = 0; triangle < numTriangles; ++triangle)
    {
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            trianglesByEdge[edgeKey(vertIdx(triangle, corner), vertIdx(triangle, corner + 1))].push_back(triangle);
        }
    }

    TriangleQuads result;
    result.quadIdxs.reserve(numTriangles * 4);
    std::vector<bool> isUsed(numTriangles, false);
    std::vector<uint32_t> loneTriangles;
    for (uint32_t triangle = 0; triangle < numTriangles; ++triangle)
    {
        if (isUsed[triangle])
        {
            continue;
        }
        isUsed[triangle] = true;

        // The lowest-index candidate, since exporters emit the two halves of a split quad consecutively
        uint32_t partner = UINT32_MAX;
        uint32_t sharedCorner = 0; // the shared edge runs from this corner to the next one
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const auto it = trianglesByEdge.find(edgeKey(vertIdx(triangle, corner + 1), vertIdx(triangle, corner)));
            if (it == trianglesByEdge.end())
            {
                continue;
            }
            for (const uint32_t candidate : it->second)
            {
                if (!isUsed[candidate])
                {
                    if (candidate < partner)
                    {
                        partner = candidate;
                        sharedCorner = corner;
                    }
                    break;
                }
            }
        }

        if (partner == UINT32_MAX)
        {
            loneTriangles.push_back(triangle);
            continue;
        }
        isUsed[partner] = true;

        // Rotated so the shared edge is v2 -> v0; the partner then reads (v0, v2, v3)
        const uint32_t v0 = vertIdx(triangle, sharedCorner + 1);
        const uint32_t v2 = vertIdx(triangle, sharedCorner);
        uint32_t v3 = UINT32_MAX;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            if (vertIdx(partner, corner) == v0 && vertIdx(partner, corner + 1) == v2)
            {
                v3 = vertIdx(partner, corner + 2);
            }
        }
        ASSERT(v3 != UINT32_MAX);

        result.quadIdxs.insert(result.quadIdxs.end(), { v0, vertIdx(triangle, sharedCorner + 2), v2, v3 });
        ++result.numPairedQuads;
    }

    for (const uint32_t triangle : loneTriangles)
    {
        const uint32_t v2 = vertIdx(triangle, 2);
        result.quadIdxs.insert(result.quadIdxs.end(), { vertIdx(triangle, 0), vertIdx(triangle, 1), v2, v2 });
    }

    return result;
}
