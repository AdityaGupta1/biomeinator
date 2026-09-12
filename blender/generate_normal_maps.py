"""Generate tileable normal maps for a block's color textures.

Usage:
    blender --background --python-exit-code 1 --python blender/generate_normal_maps.py -- stone --strength 1 --exponent 2

Writes <texture>.normal.png beside each distinct texture referenced by the block JSON.
Shared textures affect all blocks using them; no material or block JSON is changed.
"""

import argparse
import json
import math
from pathlib import Path
import sys

import bpy
import numpy as np


BLOCKS = Path(__file__).resolve().parents[1] / "assets/blocks"
TEXTURES = BLOCKS / "textures"


def generate(name, strength, exponent, invert):
    source = bpy.data.images.load(str(TEXTURES / f"{name}.png"), check_existing=False)
    source.colorspace_settings.name = "Non-Color"
    width, height = source.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    source.pixels.foreach_get(pixels)
    rgb = pixels.reshape(height, width, 4)[..., :3]
    # Use perceptual color brightness as a height proxy, not physical reflectance.
    heights = rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    heights = (heights - heights.min()) / max(float(np.ptp(heights)), 1e-6)
    if invert:
        heights = 1.0 - heights
    # Exponents > 1 flatten brighter rock into plateaus, emphasizing dark cracks.
    # Do not blur: the source pixel boundaries define the edges of the relief.
    heights = 1.0 - (1.0 - heights) ** exponent
    dx = (np.roll(heights, -1, 1) - np.roll(heights, 1, 1)) * (strength / 2)
    dy = (np.roll(heights, -1, 0) - np.roll(heights, 1, 0)) * (strength / 2)
    # Blender pixels run bottom-up; terrain texture V runs top-down.
    normals = np.stack((-dx, dy, np.ones_like(dx)), axis=-1)
    normals /= np.linalg.norm(normals, axis=-1, keepdims=True)
    rgba = np.ones((height, width, 4), dtype=np.float32)
    rgba[..., :3] = normals * 0.5 + 0.5
    output = bpy.data.images.new(f"{name}.normal", width, height, alpha=True)
    output.colorspace_settings.name = "Non-Color"
    output.pixels.foreach_set(rgba.ravel())
    output.filepath_raw = str(TEXTURES / f"{name}.normal.png")
    output.file_format = "PNG"
    output.save()
    print(f"Generated {name}.normal.png")
    bpy.data.images.remove(source)
    bpy.data.images.remove(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("block", help="Block name from assets/blocks (e.g. stone or basalt)")
    parser.add_argument("--strength", type=float, default=1.0,
                        help="Height range in texels before differentiation; 0 gives a flat normal (default: 1)")
    parser.add_argument("--exponent", type=float, default=1.0,
                        help="Height curve 1-(1-h)^exponent; >1 emphasizes dark cracks (default: 1)")
    parser.add_argument("--invert", action="store_true", help="Treat bright colors as recesses instead of dark colors")
    parser.add_argument("--face", choices=("top", "side", "bottom"),
                        help="Only generate the texture used by this face (default: all textures)")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    if not math.isfinite(args.strength) or args.strength < 0:
        parser.error("--strength must be finite and nonnegative")
    if not math.isfinite(args.exponent) or args.exponent <= 0:
        parser.error("--exponent must be finite and positive")
    block_path = BLOCKS / f"{args.block}.json"
    if not block_path.is_file():
        parser.error(f"Block not found: {block_path}")
    textures = json.loads(block_path.read_text(encoding="utf-8")).get("textures")
    if isinstance(textures, str):
        names = [textures]
    elif isinstance(textures, dict):
        if args.face and args.face not in textures:
            parser.error(f"Block has no {args.face} texture")
        names = [textures[args.face]] if args.face else list(dict.fromkeys(textures.values()))
    else:
        parser.error("Block has no color textures")
    # Check every input before overwriting any output.
    for name in names:
        if not (TEXTURES / f"{name}.png").is_file():
            parser.error(f"Texture not found: {name}.png")
    for name in names:
        generate(name, args.strength, args.exponent, args.invert)


if __name__ == "__main__":
    main()
