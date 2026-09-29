"""Prototype cave world: shared continuous fields drive cave shape, veins, strata, terraces and
palette; biomes only choose which blocks fill that geometry, then place their structures."""

import numpy as np
from numba import njit, prange

from noise import fbm, make_perm, perlin, worley

# Cave biome targets in (temperature, humidity, depth) space; depth 0 = surface, 1 = deep.
# STONE sits at the origin as in the engine; LUSH and CRYSTALS keep their current points.
BIOMES = ["stone", "lush", "crystals", "marble", "pegmatite", "opal", "copper"]
BIOME_TARGETS = np.array([
    [0.0, 0.0, 0.5],
    [0.3, 0.3, 0.5],
    [-0.3, -0.3, 0.5],
    [0.42, 0.04, 0.45],
    [-0.36, 0.3, 0.75],
    [0.3, -0.34, 0.12],
    [0.02, -0.44, 0.2],
], dtype=np.float32)
DEPTH_WEIGHT = 0.35


@njit(cache=True, inline="always")
def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


@njit(cache=True)
def terrace_style(humidity):
    """Terraces fade in with dryness, like the surface Mesa profile's dry-climate gate."""
    return smoothstep(-0.12, -0.32, humidity)


@njit(cache=True)
def roughness_style(temperature, humidity):
    return 0.35 + 0.65 * smoothstep(0.1, 0.4, abs(temperature - humidity))


FIELD_NAMES = ("temperature", "humidity", "carve", "vein", "vein_thickness", "strata", "palette")


def shared_fields(perms, shape, origin, surface_y, climate_override):
    return dict(zip(FIELD_NAMES, _shared_fields(perms, shape, origin, surface_y, climate_override)))


@njit(parallel=True, cache=True)
def _shared_fields(perms, shape, origin, surface_y, climate_override):
    """Per-voxel shared fields. climate_override (t, h) pins the climate so a hero scene stays
    in one biome; NaN leaves it to the noise."""
    sx, sy, sz = shape
    temperature = np.empty(shape, np.float32)
    humidity = np.empty(shape, np.float32)
    carve = np.empty(shape, np.float32)
    vein = np.empty(shape, np.float32)
    vein_thickness = np.empty(shape, np.float32)
    strata = np.empty(shape, np.float32)
    palette = np.empty(shape, np.float32)
    for x in prange(sx):
        wx = origin[0] + x
        for z in range(sz):
            wz = origin[2] + z
            strata_warp = 14.0 * fbm(perms[5], wx / 120.0, 3.1, wz / 120.0, 3) + 0.06 * wx
            for y in range(sy):
                wy = origin[1] + y
                if np.isnan(climate_override[0]):
                    t = 1.1 * fbm(perms[0], wx / 120.0, wy / 90.0, wz / 120.0, 3)
                    h = 1.1 * fbm(perms[1], wx / 120.0 + 17.3, wy / 90.0, wz / 120.0, 3)
                else:
                    t = climate_override[0] + 0.08 * fbm(perms[0], wx / 120.0, wy / 120.0, wz / 120.0, 2)
                    h = climate_override[1] + 0.08 * fbm(perms[1], wx / 120.0, wy / 120.0, wz / 120.0, 2)
                temperature[x, y, z] = t
                humidity[x, y, z] = h

                # Partial height quantisation of the carve sample: bounded shelves, not a remap.
                terrace = terrace_style(h)
                step = 7.0
                cell = wy / step
                shelf = step * (np.floor(cell) + smoothstep(0.3, 0.7, cell - np.floor(cell)))
                ty = wy + terrace * (shelf - wy)
                base = fbm(perms[2], wx / 46.0, ty / 26.0, wz / 46.0, 4)
                detail = fbm(perms[3], wx / 9.0, wy / 9.0, wz / 9.0, 2)
                carve[x, y, z] = base + 0.16 * roughness_style(t, h) * detail

                # Veins: thin sheets where a warped noise crosses zero; thickness is its own field.
                wxv = wx + 12.0 * perlin(perms[4], wx / 70.0, wy / 70.0, wz / 70.0)
                vein[x, y, z] = abs(fbm(perms[4], wxv / 34.0, wy / 34.0, wz / 34.0, 3))
                vein_thickness[x, y, z] = 0.018 + 0.03 * (0.5 + 0.5 * perlin(perms[6], wx / 170.0, wy / 170.0, wz / 170.0))
                strata[x, y, z] = wy + strata_warp
                palette[x, y, z] = fbm(perms[7], wx / 150.0, wy / 150.0, wz / 150.0, 2)
    return temperature, humidity, carve, vein, vein_thickness, strata, palette


@njit(parallel=True, cache=True)
def select_biomes(temperature, humidity, surface_y, origin, targets, depth_weight, forced):
    sx, sy, sz = temperature.shape
    out = np.empty(temperature.shape, np.int8)
    for x in prange(sx):
        for y in range(sy):
            depth = min(max((surface_y - (origin[1] + y)) / 140.0, 0.0), 1.0)
            for z in range(sz):
                if forced >= 0:
                    out[x, y, z] = forced
                    continue
                best = 0
                best_d = 1e9
                for b in range(targets.shape[0]):
                    dt = temperature[x, y, z] - targets[b, 0]
                    dh = humidity[x, y, z] - targets[b, 1]
                    dd = depth - targets[b, 2]
                    d = dt * dt + dh * dh + depth_weight * dd * dd
                    if d < best_d:
                        best_d = d
                        best = b
                out[x, y, z] = best
    return out


@njit(cache=True)
def carve_threshold(y_world, surface_y, seal_depth):
    """Caves are sealed within seal_depth of the surface; openings only come from shafts."""
    fade = smoothstep(surface_y - seal_depth - 12.0, surface_y - seal_depth, y_world)
    return 0.1 + 1.5 * fade


def hash01(*values):
    h = 2166136261
    for v in values:
        h = ((h ^ (int(v) & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 2246822519) & 0xFFFFFFFF
    h ^= h >> 13
    return (h & 0xFFFFFF) / float(0xFFFFFF)


class World:
    def __init__(self, blocks_ids, shape=(144, 120, 144), origin=(0, 0, 0), seed=1, surface_y=112,
                 seal_depth=10, forced_biome=None, shafts=True, scatter_lamps=False):
        self.ids = blocks_ids
        self.shape = shape
        self.origin = np.array(origin, np.float64)
        self.seed = seed
        self.surface_y = surface_y
        self.seal_depth = seal_depth
        self.forced = BIOMES.index(forced_biome) if forced_biome else -1
        self.shafts_enabled = shafts
        self.scatter_lamps = scatter_lamps
        self.rng = np.random.default_rng(seed)
        self.perms = np.stack([make_perm(seed * 31 + i) for i in range(8)])
        self.lights = []

    # ---- generation -------------------------------------------------------------------
    def generate(self):
        override = np.array([np.nan, np.nan], np.float64)
        if self.forced >= 0:
            override = BIOME_TARGETS[self.forced, :2].astype(np.float64)
        self.fields = shared_fields(self.perms, self.shape, self.origin, self.surface_y, override)
        self.biome = select_biomes(self.fields["temperature"], self.fields["humidity"], self.surface_y,
                                   self.origin, BIOME_TARGETS, DEPTH_WEIGHT, self.forced)
        self._carve()
        self._fill()
        if self.shafts_enabled:
            self._carve_shafts()
        self._place_structures()
        return self

    def _carve(self):
        sx, sy, sz = self.shape
        ys = self.origin[1] + np.arange(sy)
        threshold = np.array([carve_threshold(y, self.surface_y, self.seal_depth) for y in ys], np.float32)
        ground = self._ground_height()
        self.margin = self.fields["carve"] - threshold[None, :, None]
        self.air = self.margin > 0
        self.air[:, :3, :] = False
        above_ground = ys[None, :, None] > ground[:, None, :]
        self.cave_air = self.air & ~above_ground
        self.air |= above_ground

    def _ground_height(self):
        sx, _, sz = self.shape
        xs = self.origin[0] + np.arange(sx)
        zs = self.origin[2] + np.arange(sz)
        g = np.zeros((sx, sz), np.float32)
        for i, x in enumerate(xs):
            for j, z in enumerate(zs):
                g[i, j] = self.surface_y + 3.0 * fbm(self.perms[3], x / 60.0, 0.5, z / 60.0, 2)
        self.ground = g
        return g

    def _fill(self):
        ids = self.ids
        f = self.fields
        blocks = np.zeros(self.shape, np.uint8)
        solid = ~self.air
        biome = self.biome
        vein = f["vein"] < f["vein_thickness"]
        fringe = (f["vein"] < f["vein_thickness"] * 2.4) & ~vein
        band = np.floor(f["strata"] / 7.0).astype(np.int64)
        band_hash = ((band * 2654435761) >> 7) & 1023
        band_frac = f["strata"] / 7.0 - band
        # Seams (opal "levels") sit on some band boundaries, ~2 blocks thick with an ore fringe.
        seam_band = band_hash % 4 == 0
        is_seam = seam_band & (band_frac < 0.28)
        seam_fringe = seam_band & ((band_frac < 0.5) | (band_frac > 0.88)) & ~is_seam

        def put(mask, block):
            blocks[solid & mask] = block

        put(biome == BIOMES.index("stone"), ids["stone"])
        put(biome == BIOMES.index("lush"), ids["stone"])
        put(biome == BIOMES.index("crystals"), ids["stone"])

        marble = biome == BIOMES.index("marble")
        ruby = f["palette"] > 0.0
        put(marble & ruby, ids["marble"])
        put(marble & ~ruby, ids["grey_marble"])
        put(marble & ruby & vein, ids["graphite_marble"])
        put(marble & ~ruby & vein, ids["lapis"])
        put(marble & ~ruby & fringe, ids["lapis_ore"])
        sparse = self.rng.random(self.shape) < 0.04
        put(marble & ruby & fringe & sparse, ids["ruby_ore"])

        peg = biome == BIOMES.index("pegmatite")
        put(peg, ids["granite"])
        grains = self._worley_mask(0.13, 11.0, salt=5)
        put(peg & grains, ids["feldspar"])

        opal = biome == BIOMES.index("opal")
        strata_opal = np.array([ids["claystone"], ids["claystone_pink"], ids["claystone"], ids["claystone_pink"],
                                ids["claystone"], ids["claystone_pink"], ids["claystone"], ids["claystone_pink"],
                                ids["claystone_rust"]], np.uint8)
        blocks[solid & opal] = strata_opal[((band_hash >> 3) % 9)[solid & opal]]
        fire_region = f["palette"] > 0.45
        put(opal & seam_fringe, ids["opal_ore"])
        put(opal & is_seam & ~fire_region, ids["opal"])
        put(opal & is_seam & fire_region, ids["fire_opal"])

        copper = biome == BIOMES.index("copper")
        strata_copper = np.array([ids["strata_cream"], ids["strata_ochre"], ids["strata_pink"],
                                  ids["strata_greygreen"], ids["strata_cream"], ids["strata_pink"]], np.uint8)
        blocks[solid & copper] = strata_copper[(band_hash % 6)[solid & copper]]
        blue_joint = (band_hash % 2) == 0
        put(copper & fringe, ids["copper_stain"])
        put(copper & vein & blue_joint, ids["azurite"])
        put(copper & vein & ~blue_joint, ids["malachite"])
        self.blocks = blocks
        self._skins()

    def _worley_mask(self, radius, scale, salt):
        sx, sy, sz = self.shape
        out = np.zeros(self.shape, bool)
        _worley_fill(out, self.origin, scale, radius, salt)
        return out

    def _exposed(self):
        """Solid voxels with any cave-air face neighbour, and the direction of that neighbour."""
        solid = self.blocks > 0
        air = ~solid
        exposed = np.zeros(self.shape, bool)
        for axis in range(3):
            for shift in (1, -1):
                exposed |= solid & np.roll(air, shift, axis)
        return exposed

    def _skins(self):
        """Crusts and ceiling accents on the exposed shell, driven by a shared patch field."""
        ids = self.ids
        exposed = self._exposed() & (np.arange(self.shape[1])[None, :, None] < self.surface_y - 4)
        patch = self._patch_field(28.0, salt=9)
        copper = exposed & (self.biome == BIOMES.index("copper"))
        crust = [ids["azurite"], ids["malachite"], ids["caledonite"]]
        choice = np.floor((patch + 1.0) * 7.0).astype(int) % 3
        for i, block in enumerate(crust):
            self.blocks[copper & (patch > 0.42) & (choice == i)] = block

        air_below = np.zeros(self.shape, bool)
        air_below[:, 1:, :] = self.blocks[:, :-1, :] == 0
        ceiling = (self.blocks > 0) & air_below
        opal_ceiling = ceiling & (self.biome == BIOMES.index("opal")) & (patch < -0.15) & \
            (np.arange(self.shape[1])[None, :, None] < self.surface_y - 6)
        self.blocks[opal_ceiling] = ids["glowworm_ceiling"]
        # Glowworm threads: beads hanging 1-4 blocks under patches of ceiling.
        hang = opal_ceiling & (self.rng.random(self.shape) < 0.015)
        for x, y, z in np.argwhere(hang):
            length = int(self.rng.integers(1, 5))
            if y - length > 0 and not self.blocks[x, y - length:y, z].any():
                self.blocks[x, y - length, z] = ids["glowworm"]
                self.lights.append((x, y - length, z))

    def _patch_field(self, scale, salt):
        out = np.empty(self.shape, np.float32)
        _patch_fill(out, self.perms[(salt % 8)], self.origin, scale)
        return out

    def _carve_shafts(self):
        """Sun shafts on a jittered grid: each tests the carve field along its own axis and only
        opens where a cave lies within reach below the sealed layer."""
        sx, sy, sz = self.shape
        cell = 36
        shallow = {BIOMES.index("opal"), BIOMES.index("copper"), BIOMES.index("marble")}
        for cx in range(0, sx, cell):
            for cz in range(0, sz, cell):
                if hash01(self.seed, cx, cz, 1) > 0.75:
                    continue
                x = int(cx + 6 + hash01(self.seed, cx, cz, 2) * (cell - 12))
                z = int(cz + 6 + hash01(self.seed, cx, cz, 3) * (cell - 12))
                if x >= sx or z >= sz:
                    continue
                column = self.cave_air[x, :, z]
                top = int(self.ground[x, z])
                below = np.nonzero(column[: top - self.seal_depth])[0]
                if len(below) == 0 or top - below.max() > self.seal_depth + 26:
                    continue
                bottom = below.max()
                if int(self.biome[x, bottom, z]) not in shallow:
                    continue
                radius = 2.0 + 2.0 * hash01(self.seed, cx, cz, 4)
                for y in range(bottom, min(sy, top + 3)):
                    wobble_x = 1.5 * np.sin(y * 0.21 + cx)
                    wobble_z = 1.5 * np.cos(y * 0.17 + cz)
                    r = radius * (1.0 + 0.25 * np.sin(y * 0.5 + cz))
                    xs = np.arange(max(0, int(x - r - 3)), min(sx, int(x + r + 4)))
                    zs = np.arange(max(0, int(z - r - 3)), min(sz, int(z + r + 4)))
                    dx = xs[:, None] - (x + wobble_x)
                    dz = zs[None, :] - (z + wobble_z)
                    inside = dx * dx + dz * dz <= r * r
                    region = self.blocks[xs[0]:xs[-1] + 1, y, zs[0]:zs[-1] + 1]
                    region[inside] = 0
                self.shafts = getattr(self, "shafts", []) + [(x, z, bottom, top)]

    # ---- structures ---------------------------------------------------------------------
    def _anchors(self, min_air, floor=True):
        """Floor (or ceiling) solid voxels with at least min_air cave air above (below)."""
        solid = self.blocks > 0
        sx, sy, sz = self.shape
        run = np.zeros(self.shape, np.int16)
        if floor:
            for y in range(sy - 2, -1, -1):
                run[:, y, :] = np.where(~solid[:, y, :], run[:, y + 1, :] + 1, 0)
            cand = solid[:, :-1, :] & (run[:, 1:, :] >= min_air)
        else:
            for y in range(1, sy):
                run[:, y, :] = np.where(~solid[:, y, :], run[:, y - 1, :] + 1, 0)
            cand = solid[:, 1:, :] & (run[:, :-1, :] >= min_air)
            cand = np.concatenate([np.zeros((sx, 1, sz), bool), cand], axis=1)
            return np.argwhere(cand & (np.arange(sy)[None, :, None] < self.surface_y - 8))
        cand = np.concatenate([cand, np.zeros((sx, 1, sz), bool)], axis=1)
        return np.argwhere(cand & (np.arange(sy)[None, :, None] < self.surface_y - 8))

    def _spread(self, anchors, spacing, chance):
        order = self.rng.permutation(len(anchors))
        chosen = []
        for i in order:
            p = anchors[i]
            if self.rng.random() > chance:
                continue
            if all(np.sum((p - q) ** 2) > spacing * spacing for q in chosen):
                chosen.append(p)
        return chosen

    def _set(self, x, y, z, block, replace_solid=False):
        sx, sy, sz = self.shape
        if not (0 <= x < sx and 0 <= y < sy and 0 <= z < sz):
            return False
        if self.blocks[x, y, z] != 0 and not replace_solid:
            return False
        self.blocks[x, y, z] = block
        return True

    def _core(self, x, y, z, block, size=1):
        """A bare emitter mound, partly sunk into the floor: the NEE-able light of a cluster."""
        for dx in range(-size, size + 1):
            for dz in range(-size, size + 1):
                if abs(dx) + abs(dz) <= size:
                    self._set(x + dx, y, z + dz, block, replace_solid=True)
        self._set(x, y + 1, z, block)
        self.lights.append((x, y, z))

    def _octahedron(self, cx, cy, cz, r, block):
        for dx in range(-r, r + 1):
            for dy in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    if abs(dx) + abs(dy) + abs(dz) <= r:
                        self._set(cx + dx, cy + dy, cz + dz, block)

    def _cube(self, cx, cy, cz, s, block):
        for dx in range(s):
            for dy in range(s):
                for dz in range(s):
                    self._set(cx + dx, cy + dy, cz + dz, block)

    def _place_structures(self):
        biome_of = lambda p: BIOMES[int(self.biome[p[0], p[1], p[2]])]
        floors = self._anchors(7, floor=True)
        ceilings = self._anchors(7, floor=False)
        for p in self._spread(floors, 28, 0.35):
            kind = biome_of(p)
            if kind == "marble":
                self._marble_cluster(p)
            elif kind == "pegmatite":
                self._tourmaline_bundle(p)
            elif kind == "copper":
                self._copper_floor(p)
        for p in self._spread(ceilings, 28, 0.2):
            kind = biome_of(p)
            if kind == "marble":
                self._marble_cluster(p, hanging=True)
            elif kind == "pegmatite":
                self._tourmaline_bundle(p, hanging=True)
            elif kind == "copper":
                self._uranium_patch(p)
        walls = self._wall_anchors()
        if self.scatter_lamps:
            for p in self._spread(walls, 14, 0.5):
                x, y, z = (int(v) for v in p[:3])
                self.blocks[x, y, z] = self.ids["lamp"]
                self.lights.append((x, y, z))
        for p in self._spread(walls, 16, 0.25):
            kind = biome_of(p)
            if kind == "copper" and self.rng.random() < 0.5:
                self._uranium_patch(p)
            elif kind == "pegmatite":
                self._jutting_crystal(p)

    def _wall_anchors(self):
        solid = self.blocks > 0
        out = []
        for axis in (0, 2):
            for shift in (1, -1):
                open_side = solid & ~np.roll(solid, -shift, axis)
                open_side &= ~np.roll(solid, -2 * shift, axis)
                idx = np.argwhere(open_side & (np.arange(self.shape[1])[None, :, None] < self.surface_y - 8))
                idx = idx[self.rng.random(len(idx)) < 0.02]
                for p in idx:
                    out.append(np.array([p[0], p[1], p[2], axis, shift]))
        return np.array(out) if out else np.zeros((0, 5), int)

    def _marble_cluster(self, p, hanging=False):
        ids = self.ids
        x, y, z = (int(v) for v in p[:3])
        ruby = self.fields["palette"][x, y, z] > 0.0
        up = -1 if hanging else 1
        self._core(x, y, z, ids["ruby_core"] if ruby else ids["gold_core"], size=1)
        for _ in range(self.rng.integers(4, 7)):
            ang = self.rng.random() * 2 * np.pi
            dist = 2.0 + self.rng.random() * 2.5
            cx = int(round(x + np.cos(ang) * dist))
            cz = int(round(z + np.sin(ang) * dist))
            if ruby:
                r = int(self.rng.integers(1, 3))
                self._octahedron(cx, y + up * r, cz, r, ids["ruby"])
            else:
                s = int(self.rng.integers(1, 4))
                base_y = y + 1 if not hanging else y - s
                self._cube(cx, base_y, cz, s, ids["pyrite"])
                if self.rng.random() < 0.5:
                    self._cube(cx + s - 1, base_y + (s if not hanging else -1), cz + 1, max(1, s - 1), ids["pyrite"])

    def _tourmaline_bundle(self, p, hanging=False):
        """Terraced column bundle (the Tarugo): stepped tops, two-tone zoning along the axis whose
        direction is random per bundle, a coloured core at the base and pocket clay around it."""
        ids = self.ids
        x, y, z = (int(v) for v in p[:3])
        up = -1 if hanging else 1
        pink_tip = self.rng.random() < 0.5
        core = ids["pink_core"] if pink_tip else ids["green_core"]
        tip_block = ids["tourmaline_pink"] if pink_tip else ids["tourmaline_green"]
        base_block = ids["tourmaline_green"] if pink_tip else ids["tourmaline_pink"]
        for dx in range(-5, 6):
            for dz in range(-5, 6):
                if dx * dx + dz * dz <= 22 and self.blocks[x + dx if 0 <= x + dx < self.shape[0] else x,
                                                           y, z + dz if 0 <= z + dz < self.shape[2] else z] != 0:
                    self._set(x + dx, y, z + dz, ids["pocket_clay"], replace_solid=True)
        self._core(x + 2, y, z - 1, core, size=1)
        lean = self.rng.normal(0, 0.12, 2)
        columns = int(self.rng.integers(5, 10))
        for _ in range(columns):
            ox = self.rng.normal(0, 1.6)
            oz = self.rng.normal(0, 1.6)
            radius = 0.9 + self.rng.random() * 1.4
            height = int(6 + self.rng.random() * 14)
            split = 0.45 + self.rng.random() * 0.2
            rot = self.rng.random() * 2 * np.pi
            for h in range(height):
                # Terraced top: the column's footprint shrinks in steps over its last few layers.
                shrink = max(0, h - (height - 4)) * 0.35
                r = radius - shrink
                if r < 0.4:
                    break
                block = base_block if h < split * height else tip_block
                cxh = x + ox + lean[0] * h
                czh = z + oz + lean[1] * h
                reach = int(np.ceil(r)) + 1
                for dx in range(-reach, reach + 1):
                    for dz in range(-reach, reach + 1):
                        px = dx + (cxh - round(cxh))
                        pz = dz + (czh - round(czh))
                        # Rounded-triangle cross-section (tourmaline is trigonal).
                        a = np.arctan2(pz, px) + rot
                        tri = r * (1.0 + 0.22 * np.cos(3 * a))
                        if px * px + pz * pz <= tri * tri:
                            self._set(int(round(cxh)) + dx, y + up * (1 + h), int(round(czh)) + dz, block)

    def _jutting_crystal(self, p):
        ids = self.ids
        x, y, z, axis, shift = (int(v) for v in p)
        pink_tip = self.rng.random() < 0.5
        tip = ids["tourmaline_pink"] if pink_tip else ids["tourmaline_green"]
        base = ids["tourmaline_green"] if pink_tip else ids["tourmaline_pink"]
        length = int(self.rng.integers(4, 9))
        rise = self.rng.uniform(0.1, 0.6)
        for i in range(length):
            cx = x + (shift * i if axis == 0 else 0)
            cz = z + (shift * i if axis == 2 else 0)
            cy = y + int(round(i * rise))
            block = base if i < length * 0.55 else tip
            for d in (-1, 0, 1):
                ox, oz = (0, d) if axis == 0 else (d, 0)
                if i < length - 1 or d == 0:
                    self._set(cx + ox, cy, cz + oz, block)
                    if i < length - 2:
                        self._set(cx + ox, cy + 1, cz + oz, block)

    def _copper_floor(self, p):
        ids = self.ids
        x, y, z = (int(v) for v in p[:3])
        if self.rng.random() < 0.3:
            # Rare Bisbee calcite/aragonite formation.
            for h in range(int(self.rng.integers(3, 7))):
                r = 2 - h * 0.3
                for dx in range(-2, 3):
                    for dz in range(-2, 3):
                        if dx * dx + dz * dz <= r * r + 0.5:
                            self._set(x + dx, y + 1 + h, z + dz, ids["bisbee_calcite"])
            return
        for _ in range(self.rng.integers(3, 7)):
            s = int(self.rng.integers(1, 3))
            self._cube(x + int(self.rng.integers(-3, 4)), y + 1, z + int(self.rng.integers(-3, 4)), s, ids["boleite"])

    def _uranium_patch(self, p):
        """Neon-green crust patch replacing exposed wall/ceiling voxels: a whole-block emitter."""
        ids = self.ids
        x, y, z = (int(v) for v in p[:3])
        r = 1.5 + self.rng.random() * 1.5
        placed = False
        for dx in range(-3, 4):
            for dy in range(-3, 4):
                for dz in range(-3, 4):
                    if dx * dx + dy * dy + dz * dz > r * r:
                        continue
                    px, py, pz = x + dx, y + dy, z + dz
                    if not (0 <= px < self.shape[0] and 0 <= py < self.shape[1] and 0 <= pz < self.shape[2]):
                        continue
                    if self.blocks[px, py, pz] == 0:
                        continue
                    if self._touches_air(px, py, pz):
                        self.blocks[px, py, pz] = ids["uranium_crust"]
                        placed = True
        if placed:
            self.lights.append((x, y, z))

    def _touches_air(self, x, y, z):
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            nx, ny, nz = x + dx, y + dy, z + dz
            if 0 <= nx < self.shape[0] and 0 <= ny < self.shape[1] and 0 <= nz < self.shape[2]:
                if self.blocks[nx, ny, nz] == 0:
                    return True
        return False


@njit(parallel=True, cache=True)
def _worley_fill(out, origin, scale, radius, salt):
    sx, sy, sz = out.shape
    for x in prange(sx):
        for y in range(sy):
            for z in range(sz):
                out[x, y, z] = worley((origin[0] + x) / scale, (origin[1] + y) / scale,
                                      (origin[2] + z) / scale, salt) < radius


@njit(parallel=True, cache=True)
def _patch_fill(out, perm, origin, scale):
    sx, sy, sz = out.shape
    for x in prange(sx):
        for y in range(sy):
            for z in range(sz):
                out[x, y, z] = fbm(perm, (origin[0] + x) / scale, (origin[1] + y) / scale,
                                   (origin[2] + z) / scale, 3)
