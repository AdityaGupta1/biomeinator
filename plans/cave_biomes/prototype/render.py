"""Small CPU voxel path tracer for looks, not accuracy.

Engine parity where it matters for judging the designs: only exposed EMITTER faces are
light-sampled (NEE), glass occludes shadow rays, and emission masks (ACCENT_EMISSIVE) are
BSDF-hit only. Glass refracts with Fresnel and Beer-Lambert absorption inside the volume."""

import numpy as np
from numba import njit, prange

from blocks import ACCENT_EMISSIVE, AIR, DIFFUSE, EMITTER, GLASS, GLOSSY, METAL, TEX

MISS_SKY = -1
MISS_OUT = -2


@njit(cache=True, inline="always")
def rand(state):
    x = state[0]
    x ^= (x << np.uint32(13)) & np.uint32(0xFFFFFFFF)
    x ^= x >> np.uint32(17)
    x ^= (x << np.uint32(5)) & np.uint32(0xFFFFFFFF)
    state[0] = x
    return (x & np.uint32(0xFFFFFF)) / 16777216.0


@njit(cache=True)
def traverse(grid, ox, oy, oz, dx, dy, dz, medium, max_t):
    """Voxel DDA. medium == 0: stop at the first non-air voxel. medium > 0: we are inside glass
    of that block id; stop at the first voxel that is not that block. Returns
    (block or MISS_*, t, vx, vy, vz, axis, step_sign)."""
    sx, sy, sz = grid.shape
    vx = int(np.floor(ox))
    vy = int(np.floor(oy))
    vz = int(np.floor(oz))
    step_x = 1 if dx > 0 else -1
    step_y = 1 if dy > 0 else -1
    step_z = 1 if dz > 0 else -1
    inv_x = 1.0 / dx if dx != 0 else 1e30
    inv_y = 1.0 / dy if dy != 0 else 1e30
    inv_z = 1.0 / dz if dz != 0 else 1e30
    t_max_x = ((vx + (1 if dx > 0 else 0)) - ox) * inv_x if dx != 0 else 1e30
    t_max_y = ((vy + (1 if dy > 0 else 0)) - oy) * inv_y if dy != 0 else 1e30
    t_max_z = ((vz + (1 if dz > 0 else 0)) - oz) * inv_z if dz != 0 else 1e30
    t_dx = abs(inv_x)
    t_dy = abs(inv_y)
    t_dz = abs(inv_z)
    t = 0.0
    axis = 1
    while t < max_t:
        if t_max_x < t_max_y and t_max_x < t_max_z:
            vx += step_x
            t = t_max_x
            t_max_x += t_dx
            axis = 0
        elif t_max_y < t_max_z:
            vy += step_y
            t = t_max_y
            t_max_y += t_dy
            axis = 1
        else:
            vz += step_z
            t = t_max_z
            t_max_z += t_dz
            axis = 2
        if vy >= sy:
            return MISS_SKY, t, vx, vy, vz, axis, 1
        if vx < 0 or vx >= sx or vy < 0 or vz < 0 or vz >= sz:
            return MISS_OUT, t, vx, vy, vz, axis, 1
        b = grid[vx, vy, vz]
        if medium == 0:
            if b != 0:
                s = step_x if axis == 0 else (step_y if axis == 1 else step_z)
                return b, t, vx, vy, vz, axis, s
        elif b != medium:
            s = step_x if axis == 0 else (step_y if axis == 1 else step_z)
            return b, t, vx, vy, vz, axis, s
    return MISS_OUT, t, vx, vy, vz, axis, 1


@njit(cache=True, inline="always")
def face_uv(px, py, pz, axis):
    if axis == 0:
        u, v = pz - np.floor(pz), py - np.floor(py)
    elif axis == 1:
        u, v = px - np.floor(px), pz - np.floor(pz)
    else:
        u, v = px - np.floor(px), py - np.floor(py)
    iu = min(int(u * TEX), TEX - 1)
    iv = min(int((1.0 - v) * TEX), TEX - 1)
    return iu, iv


@njit(cache=True)
def cosine_dir(nx, ny, nz, state):
    r1 = rand(state)
    r2 = rand(state)
    phi = 2 * np.pi * r1
    r = np.sqrt(r2)
    lx = r * np.cos(phi)
    ly = r * np.sin(phi)
    lz = np.sqrt(max(0.0, 1 - r2))
    if abs(nx) > 0.9:
        tx, ty, tz = 0.0, 1.0, 0.0
    else:
        tx, ty, tz = 1.0, 0.0, 0.0
    bx = ny * tz - nz * ty
    by = nz * tx - nx * tz
    bz = nx * ty - ny * tx
    bl = np.sqrt(bx * bx + by * by + bz * bz)
    bx /= bl
    by /= bl
    bz /= bl
    tx = by * nz - bz * ny
    ty = bz * nx - bx * nz
    tz = bx * ny - by * nx
    return (lx * tx + ly * bx + lz * nx, lx * ty + ly * by + lz * ny, lx * tz + ly * bz + lz * nz)


@njit(cache=True)
def perturb(dx, dy, dz, rough, state):
    if rough <= 0:
        return dx, dy, dz
    ux = rand(state) * 2 - 1
    uy = rand(state) * 2 - 1
    uz = rand(state) * 2 - 1
    dx += ux * rough
    dy += uy * rough
    dz += uz * rough
    l = np.sqrt(dx * dx + dy * dy + dz * dz)
    return dx / l, dy / l, dz / l


@njit(cache=True)
def sky(dx, dy, dz):
    t = max(dy, 0.0)
    return (0.35 + 0.25 * t) * 1.6, (0.55 + 0.25 * t) * 1.6, (0.85 + 0.15 * t) * 1.6


@njit(cache=True)
def direct_light(grid, px, py, pz, nx, ny, nz, lights, clusters, emission, sun, sun_e, state):
    """Diffuse-lobe NEE (without albedo/pi). A one-level stand-in for the engine's light tree:
    pick an emitter cluster by power / distance^2 (skipping clusters behind the surface),
    then a uniform face within it, then a uniform point on that face; plus the sun."""
    rr = 0.0
    rg = 0.0
    rb = 0.0
    n_clusters = clusters.shape[0]
    if n_clusters > 0:
        weights = np.empty(n_clusters)
        total = 0.0
        for c in range(n_clusters):
            vx = clusters[c, 0] - px
            vy = clusters[c, 1] - py
            vz = clusters[c, 2] - pz
            d2 = vx * vx + vy * vy + vz * vz
            w = 0.0
            if vx * nx + vy * ny + vz * nz > -clusters[c, 3]:
                w = clusters[c, 4] / max(d2, 1.0)
            weights[c] = w
            total += w
        if total > 0:
            pick = rand(state) * total
            c = 0
            acc = weights[0]
            while acc < pick and c < n_clusters - 1:
                c += 1
                acc += weights[c]
            first = int(clusters[c, 5])
            count = int(clusters[c, 6])
            i = first + min(int(rand(state) * count), count - 1)
            pdf_select = weights[c] / total / count
            lx = lights[i, 0] + 0.5
            ly = lights[i, 1] + 0.5
            lz = lights[i, 2] + 0.5
            axis = lights[i, 3]
            sgn = lights[i, 4]
            a = rand(state) - 0.5
            b = rand(state) - 0.5
            lnx = 0.0
            lny = 0.0
            lnz = 0.0
            if axis == 0:
                lx += 0.5 * sgn
                ly += a
                lz += b
                lnx = sgn
            elif axis == 1:
                ly += 0.5 * sgn
                lx += a
                lz += b
                lny = sgn
            else:
                lz += 0.5 * sgn
                lx += a
                ly += b
                lnz = sgn
            vx = lx - px
            vy = ly - py
            vz = lz - pz
            d2 = vx * vx + vy * vy + vz * vz
            d = np.sqrt(d2)
            vx /= d
            vy /= d
            vz /= d
            cos_s = vx * nx + vy * ny + vz * nz
            cos_l = -(vx * lnx + vy * lny + vz * lnz)
            if cos_s > 0 and cos_l > 0:
                hit, t, hx, hy, hz, _, _ = traverse(grid, px, py, pz, vx, vy, vz, 0, d + 1.0)
                if hx == lights[i, 0] and hy == lights[i, 1] and hz == lights[i, 2]:
                    blk = grid[hx, hy, hz]
                    g = cos_s * cos_l / max(d2, 0.25) / pdf_select
                    rr += emission[blk, 0] * g
                    rg += emission[blk, 1] * g
                    rb += emission[blk, 2] * g
    cos_sun = sun[0] * nx + sun[1] * ny + sun[2] * nz
    if cos_sun > 0:
        hit, _, _, _, _, _, _ = traverse(grid, px, py, pz, sun[0], sun[1], sun[2], 0, 1e4)
        if hit == MISS_SKY:
            rr += sun_e[0] * cos_sun * np.pi
            rg += sun_e[1] * cos_sun * np.pi
            rb += sun_e[2] * cos_sun * np.pi
    return rr / np.pi, rg / np.pi, rb / np.pi


@njit(parallel=True, cache=True)
def render(grid, kind, roughness, ior, absorb, emission, f0, texture, emission_mask, lights,
           clusters, cam, width, height, spp, max_depth, sun, sun_e, seed):
    image = np.zeros((height, width, 3), np.float32)
    cx, cy, cz, fx, fy, fz, rx, ry, rz, ux, uy, uz, tan_half = cam
    aspect = width / height
    for j in prange(height):
        state = np.zeros(1, np.uint32)
        for i in range(width):
            state[0] = np.uint32((seed * 9781 + j * 6271 + i * 13 + 1) * 2654435761 % 4294967291 + 1)
            acc_r = 0.0
            acc_g = 0.0
            acc_b = 0.0
            for _ in range(spp):
                sx_ = ((i + rand(state)) / width * 2 - 1) * tan_half * aspect
                sy_ = (1 - (j + rand(state)) / height * 2) * tan_half
                dx = fx + rx * sx_ + ux * sy_
                dy = fy + ry * sx_ + uy * sy_
                dz = fz + rz * sx_ + uz * sy_
                l = np.sqrt(dx * dx + dy * dy + dz * dz)
                dx /= l
                dy /= l
                dz /= l
                ox, oy, oz = cx, cy, cz
                tr, tg, tb = 1.0, 1.0, 1.0
                lr, lg, lb = 0.0, 0.0, 0.0
                specular = True
                medium = 0
                for depth in range(max_depth):
                    hit, t, vx, vy, vz, axis, s = traverse(grid, ox, oy, oz, dx, dy, dz, medium, 400.0)
                    px = ox + dx * t
                    py = oy + dy * t
                    pz = oz + dz * t
                    if medium > 0:
                        tr *= np.exp(-absorb[medium, 0] * t)
                        tg *= np.exp(-absorb[medium, 1] * t)
                        tb *= np.exp(-absorb[medium, 2] * t)
                    if hit == MISS_SKY:
                        if medium == 0:
                            sr, sg, sb = sky(dx, dy, dz)
                            lr += tr * sr
                            lg += tg * sg
                            lb += tb * sb
                        break
                    if hit == MISS_OUT:
                        break
                    nx = 0.0
                    ny = 0.0
                    nz = 0.0
                    if axis == 0:
                        nx = -s
                    elif axis == 1:
                        ny = -s
                    else:
                        nz = -s
                    iu, iv = face_uv(px, py, pz, axis)

                    if medium > 0 and hit == 0:
                        # Leaving glass into air: dielectric interface seen from inside.
                        eta = ior[medium]
                        cos_i = -(dx * nx + dy * ny + dz * nz)
                        sin2_t = eta * eta * (1 - cos_i * cos_i)
                        r0 = ((eta - 1) / (eta + 1)) ** 2
                        fres = 1.0 if sin2_t > 1 else r0 + (1 - r0) * (1 - np.sqrt(1 - sin2_t)) ** 5
                        # n faces back into the glass here, so +n stays inside and -n exits.
                        if rand(state) < fres:
                            dx, dy, dz = dx + 2 * cos_i * nx, dy + 2 * cos_i * ny, dz + 2 * cos_i * nz
                            ox, oy, oz = px + nx * 1e-3, py + ny * 1e-3, pz + nz * 1e-3
                        else:
                            cos_t = np.sqrt(1 - sin2_t)
                            dx = eta * dx + (eta * cos_i - cos_t) * nx
                            dy = eta * dy + (eta * cos_i - cos_t) * ny
                            dz = eta * dz + (eta * cos_i - cos_t) * nz
                            dx, dy, dz = perturb(dx, dy, dz, roughness[medium], state)
                            ox, oy, oz = px - nx * 1e-3, py - ny * 1e-3, pz - nz * 1e-3
                            medium = 0
                        continue
                    if medium > 0:
                        # Glass against another block: treat as hitting that block's surface.
                        medium = 0

                    k = kind[hit]
                    ar = texture[hit, iv, iu, 0]
                    ag = texture[hit, iv, iu, 1]
                    ab = texture[hit, iv, iu, 2]
                    ox, oy, oz = px + nx * 1e-3, py + ny * 1e-3, pz + nz * 1e-3

                    if k == EMITTER:
                        if specular:
                            lr += tr * emission[hit, 0]
                            lg += tg * emission[hit, 1]
                            lb += tb * emission[hit, 2]
                        break
                    if k == ACCENT_EMISSIVE:
                        m = emission_mask[hit, iv, iu]
                        lr += tr * emission[hit, 0] * m
                        lg += tg * emission[hit, 1] * m
                        lb += tb * emission[hit, 2] * m

                    cos_o = -(dx * nx + dy * ny + dz * nz)
                    if k == GLASS:
                        eta = 1.0 / ior[hit]
                        r0 = ((1 - ior[hit]) / (1 + ior[hit])) ** 2
                        fres = r0 + (1 - r0) * (1 - cos_o) ** 5
                        if rand(state) < fres:
                            dx, dy, dz = dx + 2 * cos_o * nx, dy + 2 * cos_o * ny, dz + 2 * cos_o * nz
                        else:
                            sin2_t = eta * eta * (1 - cos_o * cos_o)
                            cos_t = np.sqrt(1 - sin2_t)
                            dx = eta * dx + (eta * cos_o - cos_t) * nx
                            dy = eta * dy + (eta * cos_o - cos_t) * ny
                            dz = eta * dz + (eta * cos_o - cos_t) * nz
                            dx, dy, dz = perturb(dx, dy, dz, roughness[hit], state)
                            ox, oy, oz = px - nx * 1e-3, py - ny * 1e-3, pz - nz * 1e-3
                            medium = hit
                        specular = True
                        continue
                    if k == METAL:
                        fr = ar + (1 - ar) * (1 - cos_o) ** 5
                        fg = ag + (1 - ag) * (1 - cos_o) ** 5
                        fb = ab + (1 - ab) * (1 - cos_o) ** 5
                        tr *= fr
                        tg *= fg
                        tb *= fb
                        dx, dy, dz = dx + 2 * cos_o * nx, dy + 2 * cos_o * ny, dz + 2 * cos_o * nz
                        dx, dy, dz = perturb(dx, dy, dz, roughness[hit], state)
                        if dx * nx + dy * ny + dz * nz <= 0:
                            break
                        specular = True
                        continue
                    if k == GLOSSY:
                        fres = 0.04 + 0.96 * (1 - cos_o) ** 5
                        if rand(state) < fres:
                            dx, dy, dz = dx + 2 * cos_o * nx, dy + 2 * cos_o * ny, dz + 2 * cos_o * nz
                            dx, dy, dz = perturb(dx, dy, dz, roughness[hit], state)
                            if dx * nx + dy * ny + dz * nz <= 0:
                                break
                            specular = True
                            continue
                    # Diffuse lobe with NEE.
                    er, eg, eb = direct_light(grid, ox, oy, oz, nx, ny, nz, lights, clusters, emission, sun, sun_e, state)
                    lr += tr * ar * er
                    lg += tg * ag * eg
                    lb += tb * ab * eb
                    tr *= ar
                    tg *= ag
                    tb *= ab
                    dx, dy, dz = cosine_dir(nx, ny, nz, state)
                    specular = False
                    if depth >= 3:
                        p = max(tr, max(tg, tb))
                        if rand(state) > p:
                            break
                        tr /= p
                        tg /= p
                        tb /= p
                acc_r += lr
                acc_g += lg
                acc_b += lb
            image[j, i, 0] = acc_r / spp
            image[j, i, 1] = acc_g / spp
            image[j, i, 2] = acc_b / spp
    return image


def light_faces(grid, kind, emission):
    """Emitter faces bordering air, grouped into clusters (connected emitter voxels).
    Returns faces sorted by cluster and per-cluster rows of
    (centre x, y, z, radius, power, first face, face count)."""
    emitters = kind[grid] == EMITTER
    coords = [tuple(p) for p in np.argwhere(emitters)]
    emitter_set = set(coords)
    label = {}
    n_clusters = 0
    for start in coords:
        if start in label:
            continue
        stack = [start]
        label[start] = n_clusters
        while stack:
            x, y, z = stack.pop()
            for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                q = (x + d[0], y + d[1], z + d[2])
                if q in emitter_set and q not in label:
                    label[q] = n_clusters
                    stack.append(q)
        n_clusters += 1
    faces = []
    for axis in range(3):
        for sign in (1, -1):
            neighbour_air = np.roll(grid, -sign, axis) == AIR
            edge = [slice(None)] * 3
            edge[axis] = -1 if sign == 1 else 0
            neighbour_air[tuple(edge)] = False
            for p in np.argwhere(emitters & neighbour_air):
                faces.append((label[tuple(p)], p[0], p[1], p[2], axis, sign))
    faces.sort()
    faces = np.array(faces, np.int32).reshape(-1, 6)
    clusters = []
    luminance = emission @ np.array([0.2126, 0.7152, 0.0722])
    for c in range(n_clusters):
        rows = np.nonzero(faces[:, 0] == c)[0]
        if len(rows) == 0:
            continue
        pts = faces[rows, 1:4] + 0.5
        centre = pts.mean(0)
        radius = np.sqrt(((pts - centre) ** 2).sum(1)).max() + 1.0
        power = luminance[grid[tuple(faces[rows[0], 1:4])]] * len(rows)
        clusters.append((*centre, radius, power, rows[0], len(rows)))
    return faces[:, 1:].copy(), np.array(clusters, np.float64).reshape(-1, 7)


def camera(pos, target, fov_deg=70):
    pos = np.asarray(pos, np.float64)
    fwd = np.asarray(target, np.float64) - pos
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    return np.array([*pos, *fwd, *right, *up, np.tan(np.radians(fov_deg) / 2)], np.float64)


def tonemap(hdr, exposure=None):
    lum = hdr @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    if exposure is None:
        # Anchor on a high percentile so bright emitters and lit walls don't blow out.
        exposure = 0.8 / max(np.percentile(lum, 94), 1e-4)
    x = hdr * exposure
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    mapped = np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)
    srgb = np.where(mapped <= 0.0031308, mapped * 12.92, 1.055 * mapped ** (1 / 2.4) - 0.055)
    return (np.clip(srgb, 0, 1) * 255).astype(np.uint8), exposure
