_Last edited: 2026-10-03_

# App icon

`src/resources/biomeinator.ico` is embedded through `biomeinator.rc`. The window class loads the
small icon separately at `SM_CXSMICON`; without `hIconSm`, Windows downscales the large icon for
the title bar, which blurs the pixel art.

The `.rc` lists the `.ico` as an `OBJECT_DEPENDS` in CMake because Ninja's resource dependency
scan only follows `#include`s, not files named by `ICON` statements; without it, replacing only
the `.ico` leaves a stale `.res` under the Ninja presets.

## Where the picture comes from

The planet is a real BiomeScanner map, not hand-drawn: seed **787259**, sampled at a 64-block
step over blocks x ∈ [-7168, 1024), z ∈ [-4416, 3776), generated with the biome code at main
commit **`752f6c6f`** (Prototype snow layers, #407). Later biome changes alter the scanner output
for that seed, so regenerating on a newer commit gives a different world. The source map is one
[BiomeScanner](../terrain/biome_scanner.md) request at that commit:
`/api/biomes?seed=787259&x0=-7168&z0=-4416&w=128&h=128&step=64`.

The map was post-processed for legibility, so the icon is not a faithful scanner render:

- Small biome patches merged into neighbours (5×5 mode filter).
- A softer palette than the scanner's (lighter forest, calmer deserts, brighter ocean); beaches
  share one sand colour.
- The polar ice cap above ~62° latitude is painted on; the map itself has no poles.
- The map is wrapped orthographically onto the visible hemisphere with a slight tilt and banded
  lighting from the upper left.

## Pixel-art constraints

Each size is drawn on its own pixel grid and only ever scaled by whole numbers (16, 20, 24, 32 native;
40 = 20×2, 48 = 24×2, 64 = 32×2, 256 = 32×8). Resampling a 32-grid sprite to 48 or 40 blurs it.
Each circle has radius n/2 on its n-pixel grid, with pixel centres tested against it; at these
sizes that makes the edge staircase's runs shrink monotonically toward the diagonal (other radii
give lumpy circles). The outline is the 1 px 4-connected inner border.

No generator script is kept; the committed `.ico` is the reference for exact colours and shading.
