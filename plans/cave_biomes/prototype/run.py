"""Prototype driver.

  run.py gen <scene>                 generate and cache a world (out/<scene>/world.npz) + cross-sections
  run.py views <scene> [--count N]   search camera candidates and render a preview contact sheet
  run.py shot <scene> <view#> [--w W --h H --spp S]   final render of a chosen view
"""

import argparse
import json
import pickle
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import blocks as blockdefs
from render import camera, light_faces, render, tonemap, traverse
from world import BIOMES, World

OUT = Path(__file__).resolve().parent / "out"

SCENES = {
    "marble": dict(forced_biome="marble", seed=3, surface_y=112),
    "pegmatite": dict(forced_biome="pegmatite", seed=5, surface_y=112, shafts=False),
    "opal": dict(forced_biome="opal", seed=11, surface_y=100, features=["opal", "fire_opal", "glowworm_ceiling"]),
    "opal_lamps": dict(forced_biome="opal", seed=11, surface_y=100, scatter_lamps=True,
                       features=["opal", "fire_opal", "glowworm_ceiling"]),
    "copper": dict(forced_biome="copper", seed=13, surface_y=100, features=["azurite", "boleite", "bisbee_calcite"]),
    "mixed": dict(forced_biome=None, seed=21, surface_y=112, shape=(256, 120, 256)),
}

SUN = np.array([0.15, 0.97, 0.12])
SUN = SUN / np.linalg.norm(SUN)
SUN_E = np.array([5.0, 4.6, 4.0], np.float64)


def scene_dir(name):
    d = OUT / name
    d.mkdir(parents=True, exist_ok=True)
    return d


def generate(name):
    block_list, ids = blockdefs.build_blocks()
    config = dict(SCENES[name])
    start = time.time()
    world = World(ids, **{k: v for k, v in config.items() if k != "features"}).generate()
    print(f"generated {name} in {time.time() - start:.1f}s; lights {len(world.lights)}")
    d = scene_dir(name)
    np.savez_compressed(d / "world.npz", blocks=world.blocks, biome=world.biome)
    with open(d / "meta.pkl", "wb") as f:
        pickle.dump(dict(lights=world.lights, shafts=getattr(world, "shafts", []), config=config), f)
    save_slices(name, world, block_list)
    return world


def average_colors(block_list):
    colors = np.array([b.texture.reshape(-1, 3).mean(0) for b in block_list])
    colors[0] = (0.02, 0.02, 0.03)
    return (np.clip(colors, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def save_slices(name, world, block_list):
    colors = average_colors(block_list)
    d = scene_dir(name)
    sx, sy, sz = world.shape
    panels = []
    for z in (sz // 4, sz // 2, 3 * sz // 4):
        panels.append(colors[world.blocks[:, :, z].T[::-1]])
    image = np.concatenate([np.pad(p, ((2, 2), (2, 2), (0, 0)), constant_values=255) for p in panels], axis=1)
    Image.fromarray(image).resize((image.shape[1] * 3, image.shape[0] * 3), Image.NEAREST).save(d / "slices_vertical.png")
    ys = [int(world.surface_y - 30), int(world.surface_y - 55), int(world.surface_y - 80)]
    panels = [colors[world.blocks[:, y, :].T] for y in ys if 0 <= y < sy]
    image = np.concatenate([np.pad(p, ((2, 2), (2, 2), (0, 0)), constant_values=255) for p in panels], axis=1)
    Image.fromarray(image).resize((image.shape[1] * 3, image.shape[0] * 3), Image.NEAREST).save(d / "slices_horizontal.png")
    palette = np.array([[120, 120, 120], [70, 160, 70], [90, 60, 150], [235, 235, 235], [220, 120, 160],
                        [90, 200, 200], [200, 130, 50]], np.uint8)
    b = palette[world.biome[:, :, sz // 2].T[::-1]]
    b[world.blocks[:, :, sz // 2].T[::-1] == 0] //= 3
    Image.fromarray(b).resize((sx * 3, sy * 3), Image.NEAREST).save(d / "biomes_vertical.png")


def load(name):
    d = scene_dir(name)
    data = np.load(d / "world.npz")
    with open(d / "meta.pkl", "rb") as f:
        meta = pickle.load(f)
    return data["blocks"], data["biome"], meta


def openness(grid, p, reach=8):
    best = reach
    for dx, dy, dz in [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1),
                       (1, 1, 0), (-1, 1, 0), (0, 1, 1), (0, 1, -1), (1, -1, 1), (-1, -1, -1)]:
        v = np.array([dx, dy, dz], float)
        v /= np.linalg.norm(v)
        hit, t, *_ = traverse(grid, p[0], p[1], p[2], v[0], v[1], v[2], 0, reach)
        if hit != -2 and hit != -1:
            best = min(best, t)
    return best


def line_of_sight(grid, a, b):
    v = np.asarray(b, float) - np.asarray(a, float)
    dist = np.linalg.norm(v)
    v /= dist
    hit, t, *_ = traverse(grid, a[0], a[1], a[2], v[0], v[1], v[2], 0, dist)
    return t >= dist - 1.5


def find_views(name, count):
    grid, biome, meta = load(name)
    rng = np.random.default_rng(1)
    targets = [np.array(p, float) + 0.5 for p in meta["lights"]]
    targets += [np.array([x + 0.5, bottom + 2.0, z + 0.5]) for x, z, bottom, top in meta["shafts"]]
    if len(targets) < count:
        # No emitters (sun-lit biomes): aim at exposed feature blocks instead.
        block_list, ids = blockdefs.build_blocks()
        feature = np.isin(grid, [ids[k] for k in meta["config"].get("features", [])])
        exposed = feature & ((np.roll(grid, 1, 1) == 0) | (np.roll(grid, -1, 0) == 0) | (np.roll(grid, 1, 2) == 0))
        cand = np.argwhere(exposed & (np.arange(grid.shape[1])[None, :, None] < meta["config"]["surface_y"] - 10))
        if len(cand):
            targets += [c + 0.5 for c in cand[rng.choice(len(cand), min(len(cand), count * 6), replace=False)]]
    rng.shuffle(targets)
    views = []
    for target in targets[: count * 4]:
        best = None
        for _ in range(120):
            direction = rng.normal(size=3)
            direction[1] = abs(direction[1]) * 0.5 + 0.1
            direction /= np.linalg.norm(direction)
            pos = target + direction * rng.uniform(12, 26)
            ip = pos.astype(int)
            if not (0 <= ip[0] < grid.shape[0] and 1 <= ip[1] < grid.shape[1] - 1 and 0 <= ip[2] < grid.shape[2]):
                continue
            if grid[tuple(ip)] != 0 or ip[1] > meta["config"]["surface_y"] - 8:
                continue
            if not line_of_sight(grid, pos, target):
                continue
            score = openness(grid, pos) + rng.random()
            if best is None or score > best[0]:
                best = (score, pos, target)
        if best and best[0] > 2.5:
            views.append(dict(pos=best[1].tolist(), target=best[2].tolist()))
        if len(views) >= count:
            break
    with open(scene_dir(name) / "views.json", "w") as f:
        json.dump(views, f, indent=1)
    return views


def render_view(name, view, width, height, spp, max_depth=6):
    grid, _, _ = load(name)
    block_list, _ = blockdefs.build_blocks()
    packed = blockdefs.pack(block_list)
    lights, clusters = light_faces(grid, packed["kind"], packed["emission"])
    cam = camera(view["pos"], view["target"], view.get("fov", 72))
    start = time.time()
    hdr = render(grid, packed["kind"], packed["roughness"], packed["ior"], packed["absorb"], packed["emission"],
                 packed["f0"], packed["texture"], packed["emission_mask"], lights, clusters, cam, width, height, spp,
                 max_depth, SUN, SUN_E, 1)
    print(f"rendered {width}x{height}@{spp} in {time.time() - start:.1f}s, {len(lights)} light faces in {len(clusters)} clusters")
    return hdr


def views_command(name, count):
    views = find_views(name, count)
    tiles = []
    for i, view in enumerate(views):
        hdr = render_view(name, view, 240, 135, 12, max_depth=4)
        ldr, _ = tonemap(hdr)
        tile = Image.fromarray(ldr)
        ImageDraw.Draw(tile).text((4, 2), str(i), fill=(255, 255, 0))
        tiles.append(tile)
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (240 * cols, 135 * rows))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % cols) * 240, (i // cols) * 135))
    sheet.save(scene_dir(name) / "views.png")
    print(f"{len(views)} views -> {scene_dir(name) / 'views.png'}")


def shot_command(name, index, width, height, spp, adjust):
    with open(scene_dir(name) / "views.json") as f:
        view = json.load(f)[index]
    if adjust:
        view.update(json.loads(adjust))
    hdr = render_view(name, view, width, height, spp)
    np.save(scene_dir(name) / f"shot_{index}.npy", hdr)
    ldr, exposure = tonemap(hdr)
    path = scene_dir(name) / f"shot_{index}.png"
    Image.fromarray(ldr).save(path)
    print(f"exposure {exposure:.3f} -> {path}")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("scene")
    v = sub.add_parser("views")
    v.add_argument("scene")
    v.add_argument("--count", type=int, default=9)
    s = sub.add_parser("shot")
    s.add_argument("scene")
    s.add_argument("index", type=int)
    s.add_argument("--w", type=int, default=960)
    s.add_argument("--h", type=int, default=540)
    s.add_argument("--spp", type=int, default=128)
    s.add_argument("--adjust", help='JSON overrides for the view, e.g. {"fov": 60}')
    args = parser.parse_args()
    if args.cmd == "gen":
        generate(args.scene)
    elif args.cmd == "views":
        views_command(args.scene, args.count)
    else:
        shot_command(args.scene, args.index, args.w, args.h, args.spp, args.adjust)


if __name__ == "__main__":
    main()
