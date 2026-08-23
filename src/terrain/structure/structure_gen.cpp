// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

// StructureGen is pure config and variant selection with no placement dependencies; it lives in
// its own translation unit so tools that only need the biome table (which contains StructureGens)
// can link it without pulling in structure placement and chunk code.

#include "structure.h"

StructureGen::StructureGen(StructureType type, uint32_t gridCellSideLength, uint32_t gridCellPadding, uint32_t flags)
    : StructureGen(std::vector<StructureGenVariant>{ { type } }, gridCellSideLength, gridCellPadding, flags)
{}

StructureGen::StructureGen(std::vector<StructureGenVariant> variants,
                           uint32_t gridCellSideLength,
                           uint32_t gridCellPadding,
                           uint32_t flags)
    : variants(std::move(variants)), gridCellSideLength(gridCellSideLength), gridCellPadding(gridCellPadding),
      flags(flags)
{}

StructureType StructureGen::pickVariant(RandomNumberGenerator& rng) const
{
    float totalWeight = 0.f;
    for (const StructureGenVariant& variant : variants)
    {
        totalWeight += variant.weight;
    }

    float roll = rng.nextFloat(totalWeight);
    for (const StructureGenVariant& variant : variants)
    {
        roll -= variant.weight;
        if (roll < 0.f)
        {
            return variant.type;
        }
    }
    return variants.back().type;
}

uint32_t StructureGen::gridSalt() const
{
    uint32_t salt = 0;
    for (const StructureGenVariant& variant : variants)
    {
        salt = hash(salt ^ static_cast<uint32_t>(variant.type));
    }
    return salt;
}
