// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../chunk.h"
#include <algorithm>
#include <iterator>
#include <numeric>
#include <tuple>

using namespace glm;

namespace
{

struct PlacementReach
{
    int geometry = 0;
    float spacingXZ = 0.f;
    float spacingY = 0.f;
};

PlacementReach placementReach(const StructureGen& gen)
{
    PlacementReach reach;
    int fitReach = 0;
    ASSERT(!gen.variants.empty());
    for (const auto& variant : gen.variants)
    {
        const auto& bounds = Structures::getStructureBounds(variant.type);
        const ivec2 extent = max(abs(bounds.minDiffXZ), abs(bounds.maxDiffXZ));
        reach.geometry = max(reach.geometry, max(extent.x, extent.y));
        const auto& fit = variant.surfaceFit;
        fitReach = max(fitReach, static_cast<int>(max(fit.clearanceRadius, fit.supportRadius)));
        reach.spacingXZ = max(reach.spacingXZ, fit.spacingXZ);
        reach.spacingY = max(reach.spacingY, fit.spacingY);
        ASSERT(fit.height > 0 && fit.spacingXZ > 0.f && fit.spacingY > 0.f && variant.weight > 0.f);
        ASSERT(fit.minSupportBlocks > 0 && fit.minSupportBlocks <= (2 * fit.supportRadius + 1) * (2 * fit.supportRadius + 1));
    }
    // A tree touching this chunk can be owned by a neighbor. Its competitors and
    // their fit probes must STILL fit inside this chunk's ready terrain halo.
    // Enforce this when opting larger structures into the mode, rather than reading
    // a neighbor's potentially unready neighborhood or silently clipping checks.
    ASSERT(reach.geometry + static_cast<int>(ceil(reach.spacingXZ)) + fitReach <=
        static_cast<int>(structureMaxChunkRadius * chunkSizeXZ), "surface placement exceeds ready terrain halo");
    return reach;
}

bool fitsSurface(const Chunk& chunk, const SurfaceStructureCandidate& candidate, const StructureSurfaceFit& fit)
{
    if (candidate.headroom < fit.height)
    {
        return false;
    }
    const ivec3 p = candidate.pos_WS;
    if (!chunk.isTerrainSolidCube_WS(p - ivec3(0, 1, 0)))
    {
        return false;
    }

    uint32_t supported = 0;
    const int supportRadius = static_cast<int>(fit.supportRadius);
    for (int z = -supportRadius; z <= supportRadius; ++z)
    {
        for (int x = -supportRadius; x <= supportRadius; ++x)
        {
            // Allow one block of unevenness without accepting a floating tree.
            supported += chunk.isTerrainSolidCube_WS(p + ivec3(x, -1, z)) ||
                         chunk.isTerrainSolidCube_WS(p + ivec3(x, -2, z));
        }
    }
    if (supported < fit.minSupportBlocks)
    {
        return false;
    }

    const int radius = static_cast<int>(fit.clearanceRadius);
    for (int z = -radius; z <= radius; ++z)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            if (x * x + z * z > radius * radius)
            {
                continue;
            }
            for (uint32_t y = 0; y < fit.height; ++y)
            {
                if (!chunk.isTerrainAir_WS(p + ivec3(x, y, z)))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

ivec2 posXZ(const SurfaceStructureCandidate& candidate)
{
    return ivec2(candidate.pos_WS.x, candidate.pos_WS.z);
}

// Whether a chunk-local XZ position lies within margin blocks of the chunk.
bool isNearChunk(ivec2 local, int margin)
{
    return local.x >= -margin && local.y >= -margin &&
           local.x < static_cast<int>(chunkSizeXZ) + margin && local.y < static_cast<int>(chunkSizeXZ) + margin;
}

bool hasPriority(const SurfaceStructureCandidate& a, const SurfaceStructureCandidate& b)
{
    return std::tie(a.priority, a.pos_WS.x, a.pos_WS.y, a.pos_WS.z) <
           std::tie(b.priority, b.pos_WS.x, b.pos_WS.y, b.pos_WS.z);
}

} // namespace

void Chunk::placeSurfaceStructures()
{
    // Candidates only compete within their own gen. Each gen buckets its candidates on an XZ
    // grid over the neighborhood, with cells at least as wide as its largest spacing, so every
    // competitor lies in the 3x3 cells around a candidate. Buckets are ranges of one index
    // array (counting sort) rather than a vector per cell.
    struct GenGrid
    {
        const StructureGen* gen;
        PlacementReach reach;
        int cellSize;
        int cellsPerSide;
        std::vector<uint32_t> cellStarts; // cellsPerSide^2 + 1 offsets into candidateIdxs
        std::vector<uint32_t> candidateIdxs;
    };
    struct Candidate
    {
        const SurfaceStructureCandidate* source;
        uint32_t genGridIdx;
        ivec2 cell;
        bool resolved = false;
        int variant = -1; // -1 when no variant fits
    };

    constexpr int neighborhoodSize = static_cast<int>((2 * structureMaxChunkRadius + 1) * chunkSizeXZ);
    const ivec2 origin = this->chunkPos * static_cast<int>(chunkSizeXZ);
    const ivec2 neighborhoodMin = origin - static_cast<int>(structureMaxChunkRadius * chunkSizeXZ);

    // Candidates stay in neighbor order: every destination chunk fills overlapping
    // structures in the same order.
    std::vector<GenGrid> genGrids;
    std::vector<Candidate> candidates;
    for (const Chunk* neighbor : this->structureNeighbors)
    {
        for (const auto& source : neighbor->surfaceStructureCandidates)
        {
            auto genGrid = std::find_if(genGrids.begin(), genGrids.end(),
                                        [&](const GenGrid& grid) { return grid.gen == source.gen; });
            if (genGrid == genGrids.end())
            {
                const PlacementReach reach = placementReach(*source.gen);
                const int cellSize = static_cast<int>(ceil(reach.spacingXZ));
                const int cellsPerSide = (neighborhoodSize + cellSize - 1) / cellSize;
                genGrids.push_back({ source.gen, reach, cellSize, cellsPerSide,
                    std::vector<uint32_t>(cellsPerSide * cellsPerSide + 1, 0), {} });
                genGrid = std::prev(genGrids.end());
            }
            // A candidate farther than geometry + spacing from this chunk can neither reach it
            // nor suppress one that does.
            if (!isNearChunk(posXZ(source) - origin, genGrid->reach.geometry + genGrid->cellSize))
            {
                continue;
            }
            const ivec2 cell = (posXZ(source) - neighborhoodMin) / genGrid->cellSize;
            ASSERT(all(greaterThanEqual(cell, ivec2(0))) && all(lessThan(cell, ivec2(genGrid->cellsPerSide))));
            ++genGrid->cellStarts[cell.x + genGrid->cellsPerSide * cell.y + 1];
            candidates.push_back({ &source, static_cast<uint32_t>(genGrid - genGrids.begin()), cell });
        }
    }
    for (GenGrid& genGrid : genGrids)
    {
        std::partial_sum(genGrid.cellStarts.begin(), genGrid.cellStarts.end(), genGrid.cellStarts.begin());
        genGrid.candidateIdxs.resize(genGrid.cellStarts.back());
    }
    {
        std::vector<std::vector<uint32_t>> cursors;
        for (const GenGrid& genGrid : genGrids)
        {
            cursors.emplace_back(genGrid.cellStarts.begin(), genGrid.cellStarts.end() - 1);
        }
        for (uint32_t candidateIdx = 0; candidateIdx < candidates.size(); ++candidateIdx)
        {
            const Candidate& candidate = candidates[candidateIdx];
            GenGrid& genGrid = genGrids[candidate.genGridIdx];
            const int cellIdx = candidate.cell.x + genGrid.cellsPerSide * candidate.cell.y;
            genGrid.candidateIdxs[cursors[candidate.genGridIdx][cellIdx]++] = candidateIdx;
        }
    }

    const auto resolve = [this](Candidate& candidate) -> int
    {
        if (candidate.resolved)
        {
            return candidate.variant;
        }
        candidate.resolved = true;
        const auto& source = *candidate.source;
        const auto& variants = source.gen->variants;
        // Weighted reservoir among the variants that actually fit. A low shelf
        // naturally chooses a shrub without any knowledge of pine tree types.
        auto rng = initRng(source.priority ^ hash(0x5E1F17u));
        float total = 0.f;
        for (size_t i = 0; i < variants.size(); ++i)
        {
            const auto& variant = variants[i];
            if (!fitsSurface(*this, source, variant.surfaceFit))
            {
                continue;
            }
            total += variant.weight;
            if (rng.nextFloat(total) < variant.weight)
            {
                candidate.variant = static_cast<int>(i);
            }
        }
        return candidate.variant;
    };

    for (Candidate& candidate : candidates)
    {
        const auto& source = *candidate.source;
        const GenGrid& genGrid = genGrids[candidate.genGridIdx];
        const PlacementReach& reach = genGrid.reach;
        const ivec2 local = posXZ(source) - origin;
        if (!isNearChunk(local, reach.geometry))
        {
            continue;
        }
        const int variantIdx = resolve(candidate);
        if (variantIdx < 0)
        {
            continue;
        }
        const auto& variant = source.gen->variants[variantIdx];

        // Any fitting higher-priority competitor suppresses this candidate, even one that is
        // itself suppressed. This can thin more than a greedy packing would, but it keeps
        // the decision local: resolving competitors recursively could depend on arbitrarily
        // distant candidates, beyond the ready terrain halo.
        const ivec2 minCell = max(candidate.cell - 1, ivec2(0));
        const ivec2 maxCell = min(candidate.cell + 1, ivec2(genGrid.cellsPerSide - 1));
        bool accepted = true;
        for (int cellZ = minCell.y; cellZ <= maxCell.y && accepted; ++cellZ)
        {
            for (int cellX = minCell.x; cellX <= maxCell.x && accepted; ++cellX)
            {
                const uint32_t cellIdx = static_cast<uint32_t>(cellX + genGrid.cellsPerSide * cellZ);
                for (uint32_t slot = genGrid.cellStarts[cellIdx]; slot < genGrid.cellStarts[cellIdx + 1]; ++slot)
                {
                    Candidate& other = candidates[genGrid.candidateIdxs[slot]];
                    const auto& competitor = *other.source;
                    if (!hasPriority(competitor, source))
                    {
                        continue;
                    }
                    const vec3 delta = vec3(competitor.pos_WS - source.pos_WS);
                    const float distanceXZ2 = delta.x * delta.x + delta.z * delta.z;
                    if (distanceXZ2 >= reach.spacingXZ * reach.spacingXZ || abs(delta.y) >= reach.spacingY)
                    {
                        continue;
                    }
                    const int otherVariantIdx = resolve(other);
                    if (otherVariantIdx < 0)
                    {
                        continue;
                    }
                    const auto& otherFit = source.gen->variants[otherVariantIdx].surfaceFit;
                    const float spacingXZ = max(variant.surfaceFit.spacingXZ, otherFit.spacingXZ);
                    const float spacingY = max(variant.surfaceFit.spacingY, otherFit.spacingY);
                    if (distanceXZ2 / (spacingXZ * spacingXZ) + delta.y * delta.y / (spacingY * spacingY) < 1.f)
                    {
                        accepted = false;
                        break;
                    }
                }
            }
        }
        if (!accepted)
        {
            continue;
        }
        const Structure structure{ variant.type, source.pos_WS };
        this->fillStructureBlocks(&structure, 1);
        if (Chunk::isInChunkXZ(local))
        {
            this->placedSurfaceStructures.push_back(structure);
        }
    }
}
