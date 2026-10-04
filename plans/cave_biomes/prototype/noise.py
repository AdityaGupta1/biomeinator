"""Numba 3D gradient noise, fBm and Worley F1 evaluated over whole voxel grids."""

import numpy as np
from numba import njit, prange

GRADIENTS = np.array([
    [1, 1, 0], [-1, 1, 0], [1, -1, 0], [-1, -1, 0],
    [1, 0, 1], [-1, 0, 1], [1, 0, -1], [-1, 0, -1],
    [0, 1, 1], [0, -1, 1], [0, 1, -1], [0, -1, -1],
    [1, 1, 0], [-1, 1, 0], [0, -1, 1], [0, -1, -1],
], dtype=np.float32)


def make_perm(seed):
    rng = np.random.default_rng(seed)
    perm = rng.permutation(256).astype(np.int32)
    return np.concatenate([perm, perm])


@njit(cache=True, inline="always")
def _fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


@njit(cache=True, inline="always")
def _grad(perm, ix, iy, iz, fx, fy, fz):
    h = perm[(perm[(perm[ix & 255] + iy) & 255] + iz) & 255] & 15
    return GRADIENTS[h, 0] * fx + GRADIENTS[h, 1] * fy + GRADIENTS[h, 2] * fz


@njit(cache=True)
def perlin(perm, x, y, z):
    ix = int(np.floor(x))
    iy = int(np.floor(y))
    iz = int(np.floor(z))
    fx = x - ix
    fy = y - iy
    fz = z - iz
    u = _fade(fx)
    v = _fade(fy)
    w = _fade(fz)
    n000 = _grad(perm, ix, iy, iz, fx, fy, fz)
    n100 = _grad(perm, ix + 1, iy, iz, fx - 1, fy, fz)
    n010 = _grad(perm, ix, iy + 1, iz, fx, fy - 1, fz)
    n110 = _grad(perm, ix + 1, iy + 1, iz, fx - 1, fy - 1, fz)
    n001 = _grad(perm, ix, iy, iz + 1, fx, fy, fz - 1)
    n101 = _grad(perm, ix + 1, iy, iz + 1, fx - 1, fy, fz - 1)
    n011 = _grad(perm, ix, iy + 1, iz + 1, fx, fy - 1, fz - 1)
    n111 = _grad(perm, ix + 1, iy + 1, iz + 1, fx - 1, fy - 1, fz - 1)
    x00 = n000 + u * (n100 - n000)
    x10 = n010 + u * (n110 - n010)
    x01 = n001 + u * (n101 - n001)
    x11 = n011 + u * (n111 - n011)
    y0 = x00 + v * (x10 - x00)
    y1 = x01 + v * (x11 - x01)
    return y0 + w * (y1 - y0)


@njit(cache=True)
def fbm(perm, x, y, z, octaves):
    total = 0.0
    amplitude = 1.0
    norm = 0.0
    for _ in range(octaves):
        total += amplitude * perlin(perm, x, y, z)
        norm += amplitude
        amplitude *= 0.5
        x *= 2.0
        y *= 2.0
        z *= 2.0
    return total / norm


@njit(cache=True)
def _hash3(ix, iy, iz, salt):
    h = (ix * 374761393 + iy * 668265263 + iz * 2147483647 + salt * 144665) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFFFFFF


@njit(cache=True)
def worley(x, y, z, salt):
    """Distance to the nearest jittered feature point (F1), roughly in [0, 1.5]."""
    ix = int(np.floor(x))
    iy = int(np.floor(y))
    iz = int(np.floor(z))
    best = 1e9
    for dx in range(-1, 2):
        for dy in range(-1, 2):
            for dz in range(-1, 2):
                cx = ix + dx
                cy = iy + dy
                cz = iz + dz
                h = _hash3(cx, cy, cz, salt)
                px = cx + (h & 1023) / 1023.0
                py = cy + ((h >> 10) & 1023) / 1023.0
                pz = cz + ((h >> 20) & 1023) / 1023.0
                d = (px - x) ** 2 + (py - y) ** 2 + (pz - z) ** 2
                if d < best:
                    best = d
    return np.sqrt(best)


@njit(parallel=True, cache=True)
def fbm_grid(perm, shape, origin, scale, octaves):
    """fBm sampled at voxel centers; scale is (sx, sy, sz) world-to-noise frequency."""
    out = np.empty(shape, dtype=np.float32)
    for x in prange(shape[0]):
        for y in range(shape[1]):
            for z in range(shape[2]):
                out[x, y, z] = fbm(perm,
                                   (origin[0] + x) * scale[0],
                                   (origin[1] + y) * scale[1],
                                   (origin[2] + z) * scale[2], octaves)
    return out


@njit(parallel=True, cache=True)
def fbm_grid_2d(perm, sx, sz, origin, scale, octaves):
    out = np.empty((sx, sz), dtype=np.float32)
    for x in prange(sx):
        for z in range(sz):
            out[x, z] = fbm(perm, (origin[0] + x) * scale, 0.5, (origin[2] + z) * scale, octaves)
    return out
