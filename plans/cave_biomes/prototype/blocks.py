"""Block table for the prototype: engine textures where a block already exists, procedural
16x16 textures for new ones, and per-block material parameters for the CPU renderer."""

from pathlib import Path

import numpy as np
from PIL import Image

# Repo root is three levels up (plans/cave_biomes/prototype); reuse the engine block textures.
ENGINE_TEXTURES = Path(__file__).resolve().parents[3] / "assets" / "blocks" / "textures"
TEX = 16

AIR, DIFFUSE, GLOSSY, GLASS, METAL, EMITTER, ACCENT_EMISSIVE = range(7)


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float32)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4).astype(np.float32)


def load_engine(name):
    image = Image.open(ENGINE_TEXTURES / f"{name}.png").convert("RGB")
    return srgb_to_linear(np.asarray(image, dtype=np.float32) / 255.0)


def rgb(hex_color):
    hex_color = hex_color.lstrip("#")
    return srgb_to_linear([int(hex_color[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def value_noise(rng, cells, amount):
    """Smooth-ish per-texel variation: bilinear upsample of a coarse random grid."""
    coarse = rng.random((cells + 1, cells + 1)).astype(np.float32)
    t = np.linspace(0, cells, TEX, endpoint=False, dtype=np.float32)
    i = t.astype(int)
    f = t - i
    a = coarse[i][:, i] * (1 - f)[None, :] + coarse[i][:, i + 1] * f[None, :]
    b = coarse[i + 1][:, i] * (1 - f)[None, :] + coarse[i + 1][:, i + 1] * f[None, :]
    field = a * (1 - f)[:, None] + b * f[:, None]
    return 1.0 + (field - 0.5) * 2.0 * amount


def speckled(rng, base, amount=0.08, cells=4):
    return base[None, None, :] * value_noise(rng, cells, amount)[..., None] * \
        (1.0 + (rng.random((TEX, TEX, 1)) - 0.5) * amount)


def splatter(rng, tex, color, fraction, size=1):
    tex = tex.copy()
    count = int(fraction * TEX * TEX / (size * size))
    for _ in range(count):
        x, y = rng.integers(0, TEX - size + 1, 2)
        tex[y:y + size, x:x + size] = color
    return tex


def desaturate(tex, amount, scale=1.0):
    luma = (tex * np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)).sum(-1, keepdims=True)
    return (tex * (1 - amount) + luma * amount) * scale


class Block:
    def __init__(self, name, texture, kind=DIFFUSE, roughness=0.5, ior=1.5, absorb=(0, 0, 0),
                 emission=(0, 0, 0), emission_mask=None, f0=None):
        self.name = name
        self.texture = texture.astype(np.float32)
        self.kind = kind
        self.roughness = roughness
        self.ior = ior
        # Beer-Lambert coefficients per block of distance travelled inside glass.
        self.absorb = np.asarray(absorb, dtype=np.float32)
        self.emission = np.asarray(emission, dtype=np.float32)
        self.emission_mask = emission_mask if emission_mask is not None else np.ones((TEX, TEX), np.float32)
        self.f0 = np.asarray(f0 if f0 is not None else (0.04, 0.04, 0.04), dtype=np.float32)


def glass_absorb_for(color_hex, density):
    """Absorption so that one block of travel roughly transmits the given colour."""
    c = np.clip(rgb(color_hex), 1e-3, 1.0)
    return -np.log(c) * density


def build_blocks():
    rng = np.random.default_rng(7)
    blocks = [Block("air", np.zeros((TEX, TEX, 3)), kind=AIR)]

    def add(block):
        blocks.append(block)
        return len(blocks) - 1

    ids = {}
    marble = load_engine("marble")
    white_crystal = load_engine("white_crystal")
    crystal_core = load_engine("crystal_core")

    ids["stone"] = add(Block("stone", load_engine("stone")))
    # Glossy marble: the user wants rough glossy reflection and little normal detail.
    ids["marble"] = add(Block("marble", marble, kind=GLOSSY, roughness=0.35))
    ids["grey_marble"] = add(Block("grey_marble", desaturate(marble, 0.6, 0.62) * rgb("#dfe3ea"), kind=GLOSSY, roughness=0.35))
    ids["graphite_marble"] = add(Block("graphite_marble", desaturate(marble, 1.0, 0.16), kind=GLOSSY, roughness=0.45))

    lapis = speckled(rng, rgb("#1d3a9e"), 0.18, 3)
    lapis = splatter(rng, lapis, rgb("#c9cfd6"), 0.05)
    lapis = splatter(rng, lapis, rgb("#d9b44a"), 0.05)
    ids["lapis"] = add(Block("lapis", lapis, kind=GLOSSY, roughness=0.5))
    lapis_ore = desaturate(marble, 0.6, 0.62) * rgb("#dfe3ea")
    lapis_ore = splatter(rng, lapis_ore, rgb("#2447b0"), 0.12, 3)
    lapis_ore = splatter(rng, lapis_ore, rgb("#d9b44a"), 0.04)
    ids["lapis_ore"] = add(Block("lapis_ore", lapis_ore, kind=GLOSSY, roughness=0.4))
    pyrite = speckled(rng, rgb("#d8bd6a"), 0.08, 2)
    for y in range(0, TEX, 4):
        pyrite[y, :] *= 0.8
    ids["pyrite"] = add(Block("pyrite", pyrite, kind=METAL, roughness=0.18, f0=rgb("#d8bd6a")))
    ids["ruby"] = add(Block("ruby", white_crystal * rgb("#ffd0d8"), kind=GLASS, roughness=0.05, ior=1.76,
                            absorb=glass_absorb_for("#d42848", 0.35)))
    ids["ruby_ore"] = add(Block("ruby_ore", splatter(rng, marble, rgb("#b01030"), 0.06, 2), kind=GLOSSY, roughness=0.35))
    ids["ruby_core"] = add(Block("ruby_core", desaturate(crystal_core, 1.0) * rgb("#ff9aa8"), kind=EMITTER,
                                 emission=rgb("#ffe0dc") * 9.0))
    ids["gold_core"] = add(Block("gold_core", desaturate(crystal_core, 1.0) * rgb("#ffd27a"), kind=EMITTER,
                                 emission=rgb("#ffe6bc") * 9.0))

    granite = speckled(rng, rgb("#cfc8bb"), 0.06, 4)
    granite = splatter(rng, granite, rgb("#8e8a84"), 0.18)
    granite = splatter(rng, granite, rgb("#2e2c2b"), 0.05)
    granite = splatter(rng, granite, rgb("#e2d6c7"), 0.1, 2)
    ids["granite"] = add(Block("granite", granite))
    feldspar = speckled(rng, rgb("#cdb1a0"), 0.05, 2)
    for x in range(0, TEX, 5):
        feldspar[:, x] *= 0.88
    ids["feldspar"] = add(Block("feldspar", feldspar, kind=GLOSSY, roughness=0.5))
    ids["tourmaline_green"] = add(Block("tourmaline_green", white_crystal * rgb("#e0f0e0"), kind=GLASS, roughness=0.08,
                                        ior=1.63, absorb=glass_absorb_for("#2f8f4a", 0.28)))
    ids["tourmaline_pink"] = add(Block("tourmaline_pink", white_crystal * rgb("#f7dfe6"), kind=GLASS, roughness=0.08,
                                       ior=1.63, absorb=glass_absorb_for("#e0457f", 0.28)))
    ids["pocket_clay"] = add(Block("pocket_clay", speckled(rng, rgb("#9a5a3a"), 0.12, 3)))
    ids["pink_core"] = add(Block("pink_core", desaturate(crystal_core, 1.0) * rgb("#ffb0cc"), kind=EMITTER,
                                 emission=rgb("#ffe4ee") * 8.0))
    ids["green_core"] = add(Block("green_core", desaturate(crystal_core, 1.0) * rgb("#b8ffc8"), kind=EMITTER,
                                  emission=rgb("#e6ffe8") * 8.0))

    ids["lamp"] = add(Block("lamp", load_engine("lamp"), kind=EMITTER, emission=rgb("#ffd9a0") * 12.0))
    ids["claystone"] = add(Block("claystone", load_engine("white_terracotta")))
    ids["claystone_pink"] = add(Block("claystone_pink", load_engine("terracotta")))
    ids["claystone_rust"] = add(Block("claystone_rust", load_engine("red_terracotta")))
    opal = speckled(rng, rgb("#2a6f86"), 0.1, 3)
    for color in ("#39d4a0", "#3f7df0", "#9be85a", "#e05a9a", "#f0c040"):
        opal = splatter(rng, opal, rgb(color), 0.08, 2)
    ids["opal"] = add(Block("opal", opal, kind=GLOSSY, roughness=0.08))
    opal_ore = load_engine("white_terracotta")
    for color in ("#39d4a0", "#3f7df0", "#9be85a"):
        opal_ore = splatter(rng, opal_ore, rgb(color), 0.04)
    ids["opal_ore"] = add(Block("opal_ore", opal_ore, kind=GLOSSY, roughness=0.3))
    fire = speckled(rng, rgb("#f07a1a"), 0.1, 3)
    fire = splatter(rng, fire, rgb("#6be060"), 0.05)
    ids["fire_opal"] = add(Block("fire_opal", fire, kind=GLOSSY, roughness=0.08))
    glowworm_mask = np.zeros((TEX, TEX), np.float32)
    for _ in range(9):
        x, y = rng.integers(1, TEX - 1, 2)
        glowworm_mask[y, x] = 1.0
    ids["glowworm_ceiling"] = add(Block("glowworm_ceiling", load_engine("white_terracotta"), kind=ACCENT_EMISSIVE,
                                        emission=rgb("#5fe8d0") * 4.0, emission_mask=glowworm_mask))
    # Stand-in for a tiny fully-emissive bead model on a silk thread (NEE-able in the engine).
    ids["glowworm"] = add(Block("glowworm", speckled(rng, rgb("#8ff5e2"), 0.05, 2), kind=EMITTER,
                                emission=rgb("#6ef0d8") * 0.9))

    ids["strata_ochre"] = add(Block("strata_ochre", desaturate(load_engine("orange_terracotta"), 0.45, 1.05)))
    ids["strata_pink"] = add(Block("strata_pink", load_engine("terracotta")))
    ids["strata_cream"] = add(Block("strata_cream", load_engine("white_terracotta")))
    ids["strata_greygreen"] = add(Block("strata_greygreen", speckled(rng, rgb("#8f988a"), 0.07, 3)))
    ids["azurite"] = add(Block("azurite", speckled(rng, rgb("#1f3fb8"), 0.2, 4), kind=GLOSSY, roughness=0.35))
    malachite = speckled(rng, rgb("#1f8a5a"), 0.08, 2)
    for y in range(TEX):
        if (y // 2) % 2:
            malachite[y, :] *= 0.72
    ids["malachite"] = add(Block("malachite", malachite, kind=GLOSSY, roughness=0.3))
    ids["caledonite"] = add(Block("caledonite", speckled(rng, rgb("#3fb3b0"), 0.12, 3), kind=GLOSSY, roughness=0.35))
    ids["copper_stain"] = add(Block("copper_stain", splatter(rng, load_engine("white_terracotta"), rgb("#3d9a8a"), 0.3, 2)))
    ids["boleite"] = add(Block("boleite", speckled(rng, rgb("#1b1f6e"), 0.08, 2), kind=GLOSSY, roughness=0.12))
    uranium = speckled(rng, rgb("#9cff3a"), 0.15, 4)
    ids["uranium_crust"] = add(Block("uranium_crust", uranium, kind=EMITTER, emission=rgb("#e4ffd0") * 7.0))
    ids["bisbee_calcite"] = add(Block("bisbee_calcite", speckled(rng, rgb("#cfe8ee"), 0.1, 3), kind=GLOSSY, roughness=0.25))

    return blocks, ids


def pack(blocks):
    """Flatten the block table into arrays the numba renderer can index."""
    n = len(blocks)
    return dict(
        kind=np.array([b.kind for b in blocks], np.int32),
        roughness=np.array([b.roughness for b in blocks], np.float32),
        ior=np.array([b.ior for b in blocks], np.float32),
        absorb=np.stack([b.absorb for b in blocks]).astype(np.float32),
        emission=np.stack([b.emission for b in blocks]).astype(np.float32),
        f0=np.stack([b.f0 for b in blocks]).astype(np.float32),
        texture=np.stack([b.texture for b in blocks]).astype(np.float32).reshape(n, TEX, TEX, 3),
        emission_mask=np.stack([b.emission_mask for b in blocks]).astype(np.float32),
    )
