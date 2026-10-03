_Last edited: 2026-10-03_

# App icon

`src/resources/biomeinator.ico` is embedded through `biomeinator.rc` and loaded by the window
class in `WindowManager::init`, which gives the exe, title bar, taskbar and Alt-Tab the same icon.

## Where the picture comes from

The planet is a real BiomeScanner map, not hand-drawn: seed **787259**, sampled at a 64-block
step over blocks x ∈ [-7168, 1024), z ∈ [-4416, 3776), generated with the biome code at main
commit **`752f6c6f`** (Prototype snow layers, #407). Later biome changes alter the scanner output
for that seed, so regenerating on a newer commit gives a different world.

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
Each grid's circle radius was chosen so the edge staircase's runs shrink monotonically toward
the diagonal, with a 1 px 4-connected outline; arbitrary radii give lumpy circles.

The generator script was deliberately not committed. A replacement only needs to reproduce the
steps above.
