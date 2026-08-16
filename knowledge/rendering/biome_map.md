_Last edited: 2026-08-15_

# Biome Color Map

`src/rendering/biome_map.cpp` — low-res world-XZ texture of per-biome grass tints
(`BIOME_MAP_BLOCKS_PER_TEXEL` blocks per texel), covering the render distance around the
camera. Shaders sample it for triangles flagged `TRIANGLE_FLAG_BIOME_TINT`
(see `biome_map.hlsli` and the tint application in `getMaterialBaseColor`).

## Filled From Noise, Not From Chunks

The fill re-evaluates the surface biome noise per texel center
(`BiomeNoiseField::fillBiomeRect`) instead of reading loaded chunks' `biomes` arrays.
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

## Grayscale-Multiply Tinting

Tint-masked texels are authored grayscale in `diffuse.png` (gray = original luminance
normalized so an average grass texel multiplies to exactly the tint color); the shader
just multiplies by `lerp(1, tint, mask)`. Hue comes entirely from the map, per-texel
detail survives as brightness. Editing grass texels later means authoring in grayscale —
the tint multiply is the only source of color.

The triangle flag only gates the biome map sample; *which texels* tint comes from the aux
texture's green channel (see [scene → materials_textures.md](../scene/materials_textures.md)),
so grass block side faces tint only their grass overlay, not the dirt below. The mesher
derives the flag from that same channel — `TerrainMaterials::sliceHasBiomeTint` reports
which texture slices have any mask coverage, so tinting a new block only requires painting
its aux tile. Flagged hits pay the map sample even when the hit texel's mask is zero (once
per hit, at `TexSampleCtx` build).

The tint rides in `TexSampleCtx` (alpha = active flag) rather than being applied at call
sites because `trySplitMaterial` bakes the sampled base color into `material.baseColor`
and clears the texture ID — applying inside `getMaterialBaseColor` means the split path
inherits the tint for later bounces automatically. The anyhit and emissive-light-sampling
sites pass an inactive tint: alpha cutout and emission are never tinted.
