"""
RTSL screen-space tile-cache MIS unbiasedness test.

Python emulator of src/shaders/tile_cache/tile_cache.hlsli +
src/shaders/common/light_tree_sampling.hlsli (descent + child-probs).

Validates the mixture-pdf evaluator (lcEvaluateMixturePdf) against the actual
sampler probabilities (lcSelectSubtreeRoot + selectLightFromSubtree): draws N
samples through the sampler and compares the empirical per-light pick frequency
to the analytic pdf. If the pdf is consistent with the sampler, every per-light
z-score stays small (< 5) across all configurations.

This is the screen-space port of the archived hash-grid validator
(plans/rtsl_improvements/source/plans/test_rtsl_cache_mis.py). The screen-space
cache adds a per-slot NORMAL TAG: a filled slot whose stored normal disagrees
with the shading normal beyond rtslCacheRejectNormalCos is filled-but-unusable
and must be folded into the (K - numAccepted) root mass, exactly like an empty
or dropped-light slot. The negative control here is the /numAccepted form (uses
plain uniformFrac and divides by numAccepted instead of K) — it must FAIL on a
partially-accepted cell.

Sampler draws are vectorized over all N samples per config, so 2M samples ×
~12 configs runs in seconds.
"""

import numpy as np
from dataclasses import dataclass, field

LIGHT_IDX_INVALID = 0xFFFFFFFF
LEAF_IDX_INVALID = 0xFFFFFFFF


@dataclass
class LightTreeNode:
    bbox_min: np.ndarray
    bbox_max: np.ndarray
    flux: float
    area_light_idx: int


# =============================================
# Tree construction
# =============================================

def build_tree(lights):
    """lights: [(bbox_min, bbox_max, flux), ...]. Pads to next pow2 with sentinels."""
    n = len(lights)
    m = 1
    while m < max(n, 1):
        m *= 2

    nodes = [None] * (2 * m - 1)
    leaf_base = m - 1
    light_to_leaf = {}

    for i in range(m):
        if i < n:
            bmin, bmax, flux = lights[i]
            nodes[leaf_base + i] = LightTreeNode(
                bbox_min=np.array(bmin, float),
                bbox_max=np.array(bmax, float),
                flux=float(flux),
                area_light_idx=i,
            )
            light_to_leaf[i] = leaf_base + i
        else:
            nodes[leaf_base + i] = LightTreeNode(
                bbox_min=np.zeros(3), bbox_max=np.zeros(3),
                flux=0.0, area_light_idx=LIGHT_IDX_INVALID,
            )

    for i in range(leaf_base - 1, -1, -1):
        left = nodes[2 * i + 1]
        right = nodes[2 * i + 2]
        if left.flux > 0 and right.flux > 0:
            bmin = np.minimum(left.bbox_min, right.bbox_min)
            bmax = np.maximum(left.bbox_max, right.bbox_max)
        elif left.flux > 0:
            bmin, bmax = left.bbox_min.copy(), left.bbox_max.copy()
        elif right.flux > 0:
            bmin, bmax = right.bbox_min.copy(), right.bbox_max.copy()
        else:
            bmin, bmax = np.zeros(3), np.zeros(3)
        nodes[i] = LightTreeNode(
            bbox_min=bmin, bbox_max=bmax,
            flux=left.flux + right.flux,
            area_light_idx=LIGHT_IDX_INVALID,
        )

    return nodes, leaf_base, m, light_to_leaf


# =============================================
# Geometry helpers (port of light_tree_sampling.hlsli)
# =============================================

def max_dist_along(p, d, bmin, bmax):
    dp = d * p
    m0 = d * bmin - dp
    m1 = d * bmax - dp
    return float(np.sum(np.maximum(m0, m1)))


def abs_min_dist_along(p, d, bmin, bmax):
    corners = np.array(np.meshgrid(
        [bmin[0], bmax[0]], [bmin[1], bmax[1]], [bmin[2], bmax[2]],
        indexing='ij',
    )).reshape(3, 8).T
    vals = (corners - p) @ d
    has_pos = bool(np.any(vals > 0))
    has_neg = bool(np.any(vals < 0))
    if has_pos and has_neg:
        return 0.0
    return float(np.min(np.abs(vals)))


def geom_term_bound(p, N, bmin, bmax):
    nrm_max = max_dist_along(p, N, bmin, bmax)
    if nrm_max <= 0.0:
        return 0.0
    if abs(N[0]) > abs(N[1]):
        denom = np.sqrt(N[0] * N[0] + N[2] * N[2])
        T = np.array([-N[2], 0.0, N[0]]) / denom
    else:
        denom = np.sqrt(N[1] * N[1] + N[2] * N[2])
        T = np.array([0.0, N[2], -N[1]]) / denom
    B = np.cross(N, T)
    B /= np.linalg.norm(B)
    y_amin = abs_min_dist_along(p, T, bmin, bmax)
    z_amin = abs_min_dist_along(p, B, bmin, bmax)
    return nrm_max / np.sqrt(y_amin * y_amin + z_amin * z_amin + nrm_max * nrm_max)


def dist_sq_to_bbox(x, bmin, bmax):
    closest = x - np.clip(x, bmin, bmax)
    dmin_sq = float(np.dot(closest, closest))
    farthest = np.maximum(np.abs(x - bmin), np.abs(x - bmax))
    dmax_sq = float(np.dot(farthest, farthest))
    return dmin_sq, dmax_sq


def rtsl_child_probs(c1, c2, hit_pos, hit_nor):
    core1 = geom_term_bound(hit_pos, hit_nor, c1.bbox_min, c1.bbox_max) * c1.flux if c1.flux > 0 else 0.0
    core2 = geom_term_bound(hit_pos, hit_nor, c2.bbox_min, c2.bbox_max) * c2.flux if c2.flux > 0 else 0.0
    if core1 == 0 and core2 == 0:
        return 0.0, 0.0
    if core1 == 0:
        return 0.0, 1.0
    if core2 == 0:
        return 1.0, 0.0
    d_min_1, d_max_1 = dist_sq_to_bbox(hit_pos, c1.bbox_min, c1.bbox_max)
    d_min_2, d_max_2 = dist_sq_to_bbox(hit_pos, c2.bbox_min, c2.bbox_max)
    if d_min_1 == 0 and d_min_2 == 0:
        p_min_1 = core1 / (core1 + core2)
    else:
        p_min_1 = (core1 * d_min_2) / (core1 * d_min_2 + core2 * d_min_1)
    if d_max_1 == 0 and d_max_2 == 0:
        p_max_1 = core1 / (core1 + core2)
    else:
        p_max_1 = (core1 * d_max_2) / (core1 * d_max_2 + core2 * d_max_1)
    p1 = 0.5 * (p_min_1 + p_max_1)
    return p1, 1.0 - p1


def precompute_child_probs(nodes, leaf_base, hit_pos, hit_nor):
    """Per-internal-node (p1, p2) — constant across descent / pdf eval for a
    given shading point. Sampler & pdf become pure table walks after this."""
    table = np.zeros((leaf_base, 2), dtype=np.float64)
    for i in range(leaf_base):
        left = nodes[2 * i + 1]
        right = nodes[2 * i + 2]
        p1, p2 = rtsl_child_probs(left, right, hit_pos, hit_nor)
        table[i, 0] = p1
        table[i, 1] = p2
    return table


# =============================================
# Scalar pdf evaluators (analytic side)
# =============================================

R_MAX_BELOW_ONE = np.nextafter(1.0, 0.0)


def evaluate_light_select_pdf(light_idx, child_probs, leaf_base, num_leaves, light_to_leaf):
    if num_leaves == 0:
        return 0.0
    leaf_idx = light_to_leaf.get(light_idx, LEAF_IDX_INVALID)
    if leaf_idx == LEAF_IDX_INVALID:
        return 0.0
    log_m = num_leaves.bit_length() - 1
    path_offset = leaf_idx - leaf_base
    cur = 0
    pdf = 1.0
    for d in range(log_m):
        p1, p2 = child_probs[cur]
        if p1 + p2 <= 0:
            return 0.0
        bit = (path_offset >> (log_m - 1 - d)) & 1
        if bit == 0:
            pdf *= p1
            cur = 2 * cur + 1
        else:
            pdf *= p2
            cur = 2 * cur + 2
    return pdf


def lc_effective_levels(L, num_leaves):
    log_m = num_leaves.bit_length() - 1
    return min(L, log_m)


def lc_ancestor_at(node_idx, levels):
    return ((node_idx + 1) >> levels) - 1


def lc_evaluate_subtree_pdf(subtree_root, leaf_idx, levels, child_probs, leaf_base):
    if levels == 0:
        return 1.0
    path_offset = leaf_idx - leaf_base
    cur = subtree_root
    pdf = 1.0
    for i in range(levels):
        p1, p2 = child_probs[cur]
        if p1 + p2 <= 0:
            return 0.0
        bit = (path_offset >> (levels - 1 - i)) & 1
        if bit == 0:
            pdf *= p1
            cur = 2 * cur + 1
        else:
            pdf *= p2
            cur = 2 * cur + 2
    return pdf


# =============================================
# Cache config + accept predicate
# =============================================

@dataclass
class CacheParams:
    enabled: bool
    uniform_frac: float
    levels: int
    lights_per_cell: int
    reject_normal_cos: float = 0.7
    num_leaves: int = 0


# A cache cell is a list of (light_idx, normal) of length K_MAX; empty slot is
# (LIGHT_IDX_INVALID, _). `None` cell == disoccluded / cell-missing == pure root.
def make_cell(k_max, entries):
    """entries: list of (light_idx, normal) for the leading slots."""
    cell = [(LIGHT_IDX_INVALID, np.array([0.0, 1.0, 0.0]))] * k_max
    for i, (li, nor) in enumerate(entries):
        cell[i] = (int(li), np.asarray(nor, float))
    return cell


def tc_slot_accepts(light_idx, normal, surf_nor, light_to_leaf, reject_cos):
    if light_idx == LIGHT_IDX_INVALID:
        return False
    if light_to_leaf.get(light_idx, LEAF_IDX_INVALID) == LEAF_IDX_INVALID:
        return False
    return float(np.dot(normal, surf_nor)) >= reject_cos


# =============================================
# Vectorized sampler
# =============================================

def vectorized_descend(child_probs, leaf_base, nodes, num_leaves, subtree_roots, r):
    """Returns an int array of picked light indices; -1 == null sample."""
    n = subtree_roots.shape[0]
    cur = subtree_roots.astype(np.int64).copy()
    rr = r.copy()
    p1col = child_probs[:, 0]
    p2col = child_probs[:, 1]
    dead = np.zeros(n, dtype=bool)

    # Internal nodes are indices < leaf_base. Iterate until all samples land on
    # a leaf (or die on a dead branch).
    max_iter = num_leaves.bit_length() + 2
    for _ in range(max_iter):
        active = (cur < leaf_base) & (~dead)
        if not active.any():
            break
        idx = np.where(active)[0]
        c = cur[idx]
        cp1 = p1col[c]
        cp2 = p2col[c]
        rsub = rr[idx]

        dead_here = (cp1 + cp2) <= 0.0
        go_left = rsub < cp1

        # Avoid div-by-zero on the not-taken branch; masked out below anyway.
        safe_p1 = np.where(cp1 > 0, cp1, 1.0)
        safe_p2 = np.where(cp2 > 0, cp2, 1.0)
        r_left = np.minimum(rsub / safe_p1, R_MAX_BELOW_ONE)
        r_right = np.minimum((rsub - cp1) / safe_p2, R_MAX_BELOW_ONE)

        new_cur = np.where(go_left, 2 * c + 1, 2 * c + 2)
        new_r = np.where(go_left, r_left, r_right)

        cur[idx] = new_cur
        rr[idx] = new_r
        dead[idx[dead_here]] = True

    # Resolve leaves -> light indices.
    out = np.full(n, -1, dtype=np.int64)
    landed = (~dead) & (cur >= leaf_base) & (cur < 2 * num_leaves - 1)
    leaf_light = np.array(
        [nodes[i].area_light_idx if (leaf_base <= i < 2 * num_leaves - 1) else LIGHT_IDX_INVALID
         for i in range(2 * num_leaves - 1)],
        dtype=np.int64,
    )
    li = np.where(landed, leaf_light[cur], LIGHT_IDX_INVALID)
    valid = landed & (li != LIGHT_IDX_INVALID)
    out[valid] = li[valid]
    return out


def vectorized_draw(cell, child_probs, nodes, leaf_base, num_leaves,
                    light_to_leaf, params, surf_nor, n_samples, rng):
    """Full sampler: lcSelectSubtreeRoot -> selectLightFromSubtree, vectorized."""
    K = params.lights_per_cell
    coin = rng.random(n_samples)
    slot_pick = rng.random(n_samples)
    r = rng.random(n_samples)

    subtree_roots = np.zeros(n_samples, dtype=np.int64)  # default: tree root

    cache_on = params.enabled and num_leaves != 0 and cell is not None
    if cache_on:
        take_cache = coin >= params.uniform_frac
        sub_slot = np.minimum((slot_pick * K).astype(np.int64), K - 1)
        # Per-slot accepted subtree root (or 0 if not accepted), precomputed for
        # the K active slots.
        slot_root = np.zeros(K, dtype=np.int64)
        for s in range(K):
            li, nor = cell[s]
            if tc_slot_accepts(li, nor, surf_nor, light_to_leaf, params.reject_normal_cos):
                leaf = light_to_leaf[li]
                slot_root[s] = lc_ancestor_at(leaf, lc_effective_levels(params.levels, num_leaves))
            else:
                slot_root[s] = 0
        chosen_root = slot_root[sub_slot]
        subtree_roots = np.where(take_cache, chosen_root, 0).astype(np.int64)

    return vectorized_descend(child_probs, leaf_base, nodes, num_leaves, subtree_roots, r)


# =============================================
# Analytic mixture pdf (correct) + negative control
# =============================================

def lc_evaluate_mixture_pdf(light_idx, child_probs, cell, leaf_base,
                            num_leaves, light_to_leaf, params, surf_nor):
    p_root = evaluate_light_select_pdf(light_idx, child_probs, leaf_base, num_leaves, light_to_leaf)
    if not params.enabled or num_leaves == 0 or cell is None:
        return p_root

    K = params.lights_per_cell
    L = lc_effective_levels(params.levels, num_leaves)
    query_leaf = light_to_leaf.get(light_idx, LEAF_IDX_INVALID)
    query_prefix = None if query_leaf == LEAF_IDX_INVALID else ((query_leaf - leaf_base) >> L)
    query_subtree_root = None if query_leaf == LEAF_IDX_INVALID else lc_ancestor_at(query_leaf, L)

    p_subtree_sum = 0.0
    num_accepted = 0
    for s in range(K):
        li, nor = cell[s]
        if not tc_slot_accepts(li, nor, surf_nor, light_to_leaf, params.reject_normal_cos):
            continue
        num_accepted += 1
        if query_leaf == LEAF_IDX_INVALID:
            continue
        cleaf = light_to_leaf[li]
        if ((cleaf - leaf_base) >> L) != query_prefix:
            continue
        p_subtree_sum += lc_evaluate_subtree_pdf(query_subtree_root, query_leaf, L, child_probs, leaf_base)

    uf = params.uniform_frac
    uf_prime = uf + (1.0 - uf) * (K - num_accepted) / K
    return uf_prime * p_root + (1.0 - uf) * p_subtree_sum / K


def lc_evaluate_mixture_pdf_NUMACCEPTED(light_idx, child_probs, cell, leaf_base,
                                        num_leaves, light_to_leaf, params, surf_nor):
    """Negative control: plain uniformFrac + divide by numAccepted (not K).
    Over-weights the cache on partially-accepted cells -> biased."""
    p_root = evaluate_light_select_pdf(light_idx, child_probs, leaf_base, num_leaves, light_to_leaf)
    if not params.enabled or num_leaves == 0 or cell is None:
        return p_root

    K = params.lights_per_cell
    L = lc_effective_levels(params.levels, num_leaves)
    query_leaf = light_to_leaf.get(light_idx, LEAF_IDX_INVALID)
    query_prefix = None if query_leaf == LEAF_IDX_INVALID else ((query_leaf - leaf_base) >> L)
    query_subtree_root = None if query_leaf == LEAF_IDX_INVALID else lc_ancestor_at(query_leaf, L)

    p_subtree_sum = 0.0
    num_accepted = 0
    for s in range(K):
        li, nor = cell[s]
        if not tc_slot_accepts(li, nor, surf_nor, light_to_leaf, params.reject_normal_cos):
            continue
        num_accepted += 1
        if query_leaf == LEAF_IDX_INVALID:
            continue
        cleaf = light_to_leaf[li]
        if ((cleaf - leaf_base) >> L) != query_prefix:
            continue
        p_subtree_sum += lc_evaluate_subtree_pdf(query_subtree_root, query_leaf, L, child_probs, leaf_base)

    if num_accepted == 0:
        return p_root
    return params.uniform_frac * p_root + (1.0 - params.uniform_frac) * p_subtree_sum / num_accepted


# =============================================
# Test harness
# =============================================

def run_consistency_test(label, child_probs, cell, nodes, leaf_base, num_leaves,
                         light_to_leaf, params, surf_nor, n_samples=2_000_000,
                         seed=42, z_threshold=5.0, pdf_fn=lc_evaluate_mixture_pdf):
    rng = np.random.default_rng(seed)
    params.num_leaves = num_leaves

    picks = vectorized_draw(cell, child_probs, nodes, leaf_base, num_leaves,
                            light_to_leaf, params, surf_nor, n_samples, rng)
    counts = np.bincount(picks[picks >= 0], minlength=num_leaves)
    null_count = int(np.count_nonzero(picks < 0))

    p_analytical = np.array([
        pdf_fn(j, child_probs, cell, leaf_base, num_leaves, light_to_leaf, params, surf_nor)
        for j in range(num_leaves)
    ])
    p_empirical = counts / n_samples

    max_z = 0.0
    worst = -1
    for j in range(num_leaves):
        if p_analytical[j] > 0 or p_empirical[j] > 0:
            std = np.sqrt(max(p_analytical[j] * (1 - p_analytical[j]), 1e-12) / n_samples)
            z = abs(p_empirical[j] - p_analytical[j]) / std
            if z > max_z:
                max_z = z
                worst = j

    null_predicted = max(0.0, 1.0 - p_analytical.sum())
    null_empirical = null_count / n_samples
    if null_predicted > 0 or null_empirical > 0:
        null_z = abs(null_empirical - null_predicted) / np.sqrt(
            max(null_predicted * (1 - null_predicted), 1e-12) / n_samples
        )
    else:
        null_z = 0.0

    passed = max_z < z_threshold and null_z < z_threshold
    status = "PASS" if passed else "FAIL"

    print(f"\n[{status}] {label}")
    print(f"  N = {n_samples}, sum(p_analytical) = {p_analytical.sum():.5f}, "
          f"null_predicted = {null_predicted:.5f}, null_empirical = {null_empirical:.5f}")
    print(f"  max z = {max_z:.2f} (light {worst}), null z = {null_z:.2f}")
    if not passed:
        print("  Per-light detail:")
        for j in range(num_leaves):
            if p_analytical[j] > 0.001 or p_empirical[j] > 0.001:
                std = np.sqrt(max(p_analytical[j] * (1 - p_analytical[j]), 1e-12) / n_samples)
                z = (p_empirical[j] - p_analytical[j]) / std
                print(f"    light {j:2d}: p_eval={p_analytical[j]:.5f}  p_emp={p_empirical[j]:.5f}  "
                      f"diff={p_empirical[j] - p_analytical[j]:+.5f}  z={z:+.2f}")

    return passed, max_z


def main():
    rng_seed = np.random.default_rng(12345)
    n_lights = 12
    lights = []
    for _ in range(n_lights):
        pos = np.array([
            rng_seed.uniform(-4, 4),
            rng_seed.uniform(1.5, 6.0),
            rng_seed.uniform(-4, 4),
        ])
        half = rng_seed.uniform(0.1, 0.5, size=3)
        flux = float(rng_seed.uniform(1.0, 5.0))
        lights.append((pos - half, pos + half, flux))

    nodes, leaf_base, num_leaves, light_to_leaf = build_tree(lights)
    log_m = num_leaves.bit_length() - 1
    print(f"Tree: {n_lights} lights, M={num_leaves}, leaf_base={leaf_base}, logM={log_m}")

    surf_pos = np.array([0.5, 0.3, 0.7])
    surf_nor = np.array([0.0, 1.0, 0.0])
    K_MAX = 32

    child_probs = precompute_child_probs(nodes, leaf_base, surf_pos, surf_nor)
    print(f"Precomputed child_probs table: {leaf_base} internal nodes")

    OK = surf_nor                       # passes the normal-tag cosine test
    BAD = np.array([1.0, 0.0, 0.0])     # dot == 0 < 0.7 -> normal-rejected

    results = []

    def add(label, cell, params):
        results.append((label, *run_consistency_test(
            label, child_probs, cell, nodes, leaf_base, num_leaves,
            light_to_leaf, params, surf_nor)))

    # 1. Cache disabled — pure root descent baseline.
    add("cache disabled",
        None, CacheParams(enabled=False, uniform_frac=0.5, levels=5, lights_per_cell=4))

    # 2. Disoccluded / cell missing — lookup invalid -> pure root.
    add("disoccluded (null cell)",
        None, CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4))

    # 3. Cell present, all slots empty.
    add("cell all empty",
        make_cell(K_MAX, []),
        CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4))

    # 4. Partial fill — primary /numAccepted stress test.
    add("cell 2/4 filled",
        make_cell(K_MAX, [(3, OK), (7, OK)]),
        CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4))

    # 5. Fully filled.
    add("cell 4/4 filled",
        make_cell(K_MAX, [(1, OK), (5, OK), (9, OK), (11, OK)]),
        CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4))

    # 6. Normal-rejected mix — 2 accept, 2 filled-but-normal-rejected.
    add("normal-rejected mix",
        make_cell(K_MAX, [(1, OK), (5, BAD), (9, OK), (11, BAD)]),
        CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4))

    # 7. Dropped-light + empty + normal-rejected mix.
    add("dropped + empty + reject",
        make_cell(K_MAX, [(3, OK), (99, OK), (7, BAD), (LIGHT_IDX_INVALID, OK)]),
        CacheParams(enabled=True, uniform_frac=0.2, levels=5, lights_per_cell=4))

    # 8. uniformFrac = 0 — pure cache mode.
    add("uniformFrac=0",
        make_cell(K_MAX, [(1, OK), (5, OK), (9, OK), (11, OK)]),
        CacheParams(enabled=True, uniform_frac=0.0, levels=3, lights_per_cell=4))

    # 9. uniformFrac = 1 — pure root.
    add("uniformFrac=1",
        make_cell(K_MAX, [(1, OK), (5, OK), (9, OK), (11, OK)]),
        CacheParams(enabled=True, uniform_frac=1.0, levels=3, lights_per_cell=4))

    # 10. K = K_MAX dense (32 slots), uniformFrac=0.05.
    dense = [(i % n_lights, OK if (i % 3) else BAD) for i in range(K_MAX)]
    add("K=32 dense",
        make_cell(K_MAX, dense),
        CacheParams(enabled=True, uniform_frac=0.05, levels=3, lights_per_cell=32))

    # 11. L >= logM — lcEffectiveLevels collapses subtree root to tree root.
    add("L>=logM (root collapse)",
        make_cell(K_MAX, [(1, OK), (5, OK), (9, OK), (11, OK)]),
        CacheParams(enabled=True, uniform_frac=0.2, levels=10, lights_per_cell=4))

    # =============================================
    # Negative control: /numAccepted form on a partially-accepted cell.
    # =============================================
    print("\n--- Negative control: /numAccepted form (expected FAIL) ---")
    params = CacheParams(enabled=True, uniform_frac=0.2, levels=3, lights_per_cell=4)
    params.num_leaves = num_leaves
    neg_cell = make_cell(K_MAX, [(3, OK), (7, OK)])
    neg_passed, neg_z = run_consistency_test(
        "NEG /numAccepted on 2/4 cell (expected FAIL)",
        child_probs, neg_cell, nodes, leaf_base, num_leaves,
        light_to_leaf, params, surf_nor,
        pdf_fn=lc_evaluate_mixture_pdf_NUMACCEPTED)

    # =============================================
    # Summary
    # =============================================
    print("\n" + "=" * 72)
    print("Summary:")
    n_pass = sum(1 for _, p, _ in results if p)
    n_total = len(results)
    for label, passed, max_z in results:
        marker = "PASS" if passed else "FAIL"
        print(f"  [{marker}] {label:<28}  max z = {max_z:.2f}")
    print(f"\n{n_pass}/{n_total} consistency tests passed.")
    if n_pass == n_total:
        print("==> Exact mixture pdf is consistent with the sampler. MIS unbiased.")
    else:
        print("==> MIS pdf INCONSISTENT with sampler — biased estimator.")
    print(f"\nNegative control (/numAccepted): "
          f"{'FAIL as expected' if not neg_passed else 'unexpectedly PASS'}  (max z = {neg_z:.2f})")

    all_good = (n_pass == n_total) and (not neg_passed)
    raise SystemExit(0 if all_good else 1)


if __name__ == "__main__":
    main()
