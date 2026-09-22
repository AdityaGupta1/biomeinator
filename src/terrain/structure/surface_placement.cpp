// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../chunk.h"
#include <algorithm>
#include <tuple>
#include <unordered_map>

using namespace glm;

namespace
{

struct PlacementReach
{
    int geometry = 0;
    int fit = 0;
    float spacingXZ = 0.f;
    float spacingY = 0.f;
};

PlacementReach placementReach(const StructureGen& gen)
{
    PlacementReach reach;
    ASSERT(!gen.variants.empty());
    for (const auto& variant : gen.variants)
    {
        const auto& bounds = Structures::getStructureBounds(variant.type);
        const ivec2 extent = max(abs(bounds.minDiffXZ), abs(bounds.maxDiffXZ));
        reach.geometry = max(reach.geometry, max(extent.x, extent.y));
        const auto& fit = variant.surfaceFit;
        reach.fit = max(reach.fit, static_cast<int>(max(fit.clearanceRadius, fit.supportRadius)));
        reach.spacingXZ = max(reach.spacingXZ, fit.spacingXZ);
        reach.spacingY = max(reach.spacingY, fit.spacingY);
        ASSERT(fit.height > 0 && fit.spacingXZ > 0.f && fit.spacingY > 0.f && variant.weight > 0.f);
        ASSERT(fit.minSupportBlocks > 0 && fit.minSupportBlocks <= (2 * fit.supportRadius + 1) * (2 * fit.supportRadius + 1));
    }
    // A tree touching this chunk can be owned by a neighbor. Its competitors and
    // their fit probes must STILL fit inside this chunk's ready terrain halo.
    // Enforce this when opting larger structures into the mode, rather than reading
    // a neighbor's potentially unready neighborhood or silently clipping checks.
    ASSERT(reach.geometry + static_cast<int>(ceil(reach.spacingXZ)) + reach.fit <=
        static_cast<int>(structureMaxChunkRadius * chunkSizeXZ), "surface placement exceeds ready terrain halo");
    return reach;
}

bool fitsSurface(const Chunk& chunk, const SurfaceStructureCandidate& candidate, const StructureSurfaceFit& fit)
{
    if (candidate.headroom < fit.height) return false;
    const ivec3 p = candidate.pos_WS;
    if (!chunk.isTerrainSolidCube_WS(p - ivec3(0, 1, 0))) return false;

    uint32_t supported = 0;
    const int supportRadius = static_cast<int>(fit.supportRadius);
    for (int z = -supportRadius; z <= supportRadius; ++z)
        for (int x = -supportRadius; x <= supportRadius; ++x)
        {
            // Allow one block of unevenness without accepting a floating tree.
            supported += chunk.isTerrainSolidCube_WS(p + ivec3(x, -1, z)) ||
                         chunk.isTerrainSolidCube_WS(p + ivec3(x, -2, z));
        }
    if (supported < fit.minSupportBlocks) return false;

    const int radius = static_cast<int>(fit.clearanceRadius);
    for (int z = -radius; z <= radius; ++z)
        for (int x = -radius; x <= radius; ++x)
        {
            if (x * x + z * z > radius * radius) continue;
            for (uint32_t y = 0; y < fit.height; ++y)
                if (!chunk.isTerrainAir_WS(p + ivec3(x, y, z))) return false;
        }
    return true;
}

bool hasPriority(const SurfaceStructureCandidate& a, const SurfaceStructureCandidate& b)
{
    return std::tie(a.priority, a.pos_WS.x, a.pos_WS.y, a.pos_WS.z) <
           std::tie(b.priority, b.pos_WS.x, b.pos_WS.y, b.pos_WS.z);
}

} // namespace

void Chunk::placeSurfaceStructures()
{
    struct Candidate
    {
        const SurfaceStructureCandidate* source;
        const PlacementReach* reach;
        int variant = -2; // unresolved; -1 means none fit
    };
    std::unordered_map<const StructureGen*, PlacementReach> reaches;
    std::vector<Candidate> candidates;
    for (const Chunk* neighbor : this->structureNeighbors)
        for (const auto& source : neighbor->surfaceStructureCandidates)
        {
            auto [it, inserted] = reaches.try_emplace(source.gen);
            if (inserted) it->second = placementReach(*source.gen);
            candidates.push_back({ &source, &it->second });
        }

    const auto resolve = [this](Candidate& candidate) -> int
    {
        if (candidate.variant != -2) return candidate.variant;
        const auto& source = *candidate.source;
        const auto& variants = source.gen->variants;
        // Weighted reservoir among the variants that actually fit. A low shelf
        // naturally chooses a shrub without any knowledge of pine tree types.
        auto rng = initRng(source.priority ^ hash(0x5E1F17u));
        float total = 0.f;
        candidate.variant = -1;
        for (size_t i = 0; i < variants.size(); ++i)
        {
            const auto& variant = variants[i];
            if (!fitsSurface(*this, source, variant.surfaceFit)) continue;
            total += variant.weight;
            if (rng.nextFloat(total) < variant.weight) candidate.variant = static_cast<int>(i);
        }
        return candidate.variant;
    };

    const ivec2 origin = this->chunkPos * static_cast<int>(chunkSizeXZ);
    for (Candidate& candidate : candidates)
    {
        const auto& source = *candidate.source;
        const ivec2 local = ivec2(source.pos_WS.x, source.pos_WS.z) - origin;
        const int reach = candidate.reach->geometry;
        if (local.x + reach < 0 || local.y + reach < 0 ||
            local.x - reach >= static_cast<int>(chunkSizeXZ) ||
            local.y - reach >= static_cast<int>(chunkSizeXZ)) continue;
        const int variantIdx = resolve(candidate);
        if (variantIdx < 0) continue;
        const auto& variant = source.gen->variants[variantIdx];
        bool accepted = true;
        for (Candidate& other : candidates)
        {
            const auto& neighbor = *other.source;
            if (neighbor.gen != source.gen || !hasPriority(neighbor, source)) continue;
            const vec3 delta = vec3(neighbor.pos_WS - source.pos_WS);
            const float distanceXZ2 = delta.x * delta.x + delta.z * delta.z;
            if (distanceXZ2 >= candidate.reach->spacingXZ * candidate.reach->spacingXZ ||
                abs(delta.y) >= candidate.reach->spacingY) continue;
            const int otherIdx = resolve(other);
            if (otherIdx < 0) continue;
            const auto& otherFit = source.gen->variants[otherIdx].surfaceFit;
            const float spacingXZ = max(variant.surfaceFit.spacingXZ, otherFit.spacingXZ);
            const float spacingY = max(variant.surfaceFit.spacingY, otherFit.spacingY);
            if (distanceXZ2 / (spacingXZ * spacingXZ) + delta.y * delta.y / (spacingY * spacingY) < 1.f)
            {
                accepted = false;
                break;
            }
        }
        if (!accepted) continue;
        const Structure structure{ variant.type, source.pos_WS };
        this->fillStructureBlocks(&structure, 1);
        if (Chunk::isInChunkXZ(local)) this->placedSurfaceStructures.push_back(structure);
    }
}
