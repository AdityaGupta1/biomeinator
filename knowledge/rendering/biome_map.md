_Last edited: 2026-07-29_

# Biome Color Map

`src/rendering/biome_map.cpp` — low-res world-XZ texture of per-biome grass tints
(`BIOME_MAP_BLOCKS_PER_TEXEL` blocks per texel), covering the render distance around the
camera. Shaders sample it for triangles flagged `TRIANGLE_FLAG_BIOME_TINT`
(see `biome_map.hlsli` and the tint application in `getMaterialBaseColor`).

## Filled From Noise, Not From Chunks

The fill re-evaluates the surface biome noise per texel center
(`ChunkGenerator::fillBiomeRect`) instead of reading loaded chunks' `biomes` arrays.
This avoids depending on chunk load state at the map edges, avoids racing the worker
threads that generate chunks, and skips the per-column jitter so the coarse map shows the
smooth macro biome field. Bicubic B-spline sampling (two offset hardware bilinear taps per
axis) then blends tints across biome borders over ~2 texels; plain bilinear showed visible
diamond-pattern banding at texel scale.
Consequence: a column whose jittered biome flipped can sample a neighbor biome's tint —
invisible at tint scale, but don't use this map for anything requiring exact per-column
biome identity.

## Refill and Recreate Triggers

The map refills when the texel-grid-snapped origin moves (camera crossed a texel
boundary), when the world seed changes (world import re-inits `ChunkGenerator` with the
imported seed; the seed comparison picks that up a frame later), and when
`renderDistance` changes size. On resize the old texture *and* its descriptor slot go
through the frame's `ToFreeList`, and a fresh slot is allocated — the SRV index is
rewritten into `HeapIndices` every frame, so in-flight frames keep reading the old
descriptor safely.

## Luminance-Replace Tinting

Grass textures are still authored green; no grayscale assets. Tinted faces replace the
sampled color with `luminance * tint / BIOME_TINT_REFERENCE_LUMINANCE` — hue comes
entirely from the map, per-texel detail survives as luminance. The reference constant
normalizes so a texel at that luminance renders exactly the tint color.

The tint rides in `TexSampleCtx` (alpha = active flag) rather than being applied at call
sites because `trySplitMaterial` bakes the sampled base color into `material.baseColor`
and clears the texture ID — applying inside `getMaterialBaseColor` means the split path
inherits the tint for later bounces automatically. The anyhit and emissive-light-sampling
sites pass an inactive tint: alpha cutout and emission are never tinted.
