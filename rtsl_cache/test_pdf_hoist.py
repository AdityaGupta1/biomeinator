"""
RTSL tile-cache pdf-hoist equivalence test (plan step 4, validation item 8).

lcEvaluateMixturePdf sums a per-slot subtree pdf over the K cache slots. Every
accepted slot whose cached light shares the query light's high (logM - L) prefix
descends from the SAME subtree root (the query light's L-ancestor — see the
prefix algebra in tile_cache.hlsli), so each contributes the IDENTICAL
lcEvaluateSubtreePdf value. The shader therefore hoists the walk out of the loop:

    pSubtreeSum = numMatching * lcEvaluateSubtreePdf(querySubtreeRoot, j, ...)

instead of one walk per matching slot. This test checks the hoisted (count) form
against the naive (per-slot sum) form — lc_evaluate_mixture_pdf in
test_cache_mis.py — over many random tile configurations. They are algebraically
identical (a sum of N copies of x equals N*x), so any difference is pure FP
rounding and must stay below 1e-5 relative error.
"""

import numpy as np

from test_cache_mis import (
    LIGHT_IDX_INVALID,
    LEAF_IDX_INVALID,
    CacheParams,
    build_tree,
    precompute_child_probs,
    evaluate_light_select_pdf,
    lc_effective_levels,
    lc_ancestor_at,
    lc_evaluate_subtree_pdf,
    tc_slot_accepts,
    make_cell,
    lc_evaluate_mixture_pdf,  # naive K-walk reference
)


def lc_evaluate_mixture_pdf_hoisted(light_idx, child_probs, cell, leaf_base,
                                    num_leaves, light_to_leaf, params, surf_nor):
    """Mirror of the shader's hoisted lcEvaluateMixturePdf: count matching slots,
    do ONE subtree walk."""
    p_root = evaluate_light_select_pdf(light_idx, child_probs, leaf_base, num_leaves, light_to_leaf)
    if not params.enabled or num_leaves == 0 or cell is None:
        return p_root

    K = params.lights_per_cell
    L = lc_effective_levels(params.levels, num_leaves)
    query_leaf = light_to_leaf.get(light_idx, LEAF_IDX_INVALID)
    query_prefix = None if query_leaf == LEAF_IDX_INVALID else ((query_leaf - leaf_base) >> L)
    query_subtree_root = None if query_leaf == LEAF_IDX_INVALID else lc_ancestor_at(query_leaf, L)

    num_accepted = 0
    num_matching = 0
    for s in range(K):
        li, nor = cell[s]
        if not tc_slot_accepts(li, nor, surf_nor, light_to_leaf, params.reject_normal_cos):
            continue
        num_accepted += 1
        if query_leaf == LEAF_IDX_INVALID:
            continue
        cleaf = light_to_leaf[li]
        if ((cleaf - leaf_base) >> L) == query_prefix:
            num_matching += 1

    if num_matching > 0:
        p_subtree_sum = num_matching * lc_evaluate_subtree_pdf(
            query_subtree_root, query_leaf, L, child_probs, leaf_base)
    else:
        p_subtree_sum = 0.0

    uf = params.uniform_frac
    uf_prime = uf + (1.0 - uf) * (K - num_accepted) / K
    return uf_prime * p_root + (1.0 - uf) * p_subtree_sum / K


def random_lights(rng, n_lights):
    lights = []
    for _ in range(n_lights):
        pos = np.array([
            rng.uniform(-5, 5),
            rng.uniform(1.0, 7.0),
            rng.uniform(-5, 5),
        ])
        half = rng.uniform(0.1, 0.6, size=3)
        flux = float(rng.uniform(0.5, 6.0))
        lights.append((pos - half, pos + half, flux))
    return lights


def random_cell(rng, k_max, lights_per_cell, num_leaves, surf_nor):
    """Random mix of accepted lights, dropped lights, normal-rejected, and empty
    slots so the matching/non-matching split is exercised."""
    entries = []
    n_fill = rng.integers(0, lights_per_cell + 1)
    for _ in range(int(n_fill)):
        roll = rng.random()
        if roll < 0.15:
            # dropped light (idx with no leaf)
            li = num_leaves + int(rng.integers(0, 8))
            nor = surf_nor
        elif roll < 0.30:
            # explicit empty sentinel
            li = LIGHT_IDX_INVALID
            nor = surf_nor
        elif roll < 0.50:
            # normal-rejected (orthogonal normal)
            li = int(rng.integers(0, num_leaves))
            nor = np.array([1.0, 0.0, 0.0])
        else:
            # accepted
            li = int(rng.integers(0, num_leaves))
            nor = surf_nor
        entries.append((li, nor))
    return make_cell(k_max, entries)


def main():
    rng = np.random.default_rng(20260523)
    K_MAX = 32

    n_configs = 1024
    max_rel_err = 0.0
    worst = None
    n_compared = 0

    for cfg in range(n_configs):
        n_lights = int(rng.integers(2, 24))
        lights = random_lights(rng, n_lights)
        nodes, leaf_base, num_leaves, light_to_leaf = build_tree(lights)
        log_m = num_leaves.bit_length() - 1

        surf_pos = np.array([rng.uniform(-2, 2), rng.uniform(0.0, 1.0), rng.uniform(-2, 2)])
        # Keep the shading normal roughly upward so a chunk of slots pass the cosine test.
        surf_nor = np.array([rng.uniform(-0.3, 0.3), 1.0, rng.uniform(-0.3, 0.3)])
        surf_nor /= np.linalg.norm(surf_nor)

        child_probs = precompute_child_probs(nodes, leaf_base, surf_pos, surf_nor)

        levels = int(rng.integers(1, max(2, log_m + 2)))   # span below and at logM
        lights_per_cell = int(rng.integers(1, K_MAX + 1))
        uniform_frac = float(rng.uniform(0.0, 1.0))
        params = CacheParams(enabled=True, uniform_frac=uniform_frac, levels=levels,
                             lights_per_cell=lights_per_cell)
        params.num_leaves = num_leaves

        cell = random_cell(rng, K_MAX, lights_per_cell, num_leaves, surf_nor)

        for j in range(num_leaves):
            naive = lc_evaluate_mixture_pdf(
                j, child_probs, cell, leaf_base, num_leaves, light_to_leaf, params, surf_nor)
            hoisted = lc_evaluate_mixture_pdf_hoisted(
                j, child_probs, cell, leaf_base, num_leaves, light_to_leaf, params, surf_nor)
            n_compared += 1
            denom = max(abs(naive), 1e-12)
            rel = abs(naive - hoisted) / denom
            if rel > max_rel_err:
                max_rel_err = rel
                worst = (cfg, j, naive, hoisted)

    threshold = 1e-5
    passed = max_rel_err < threshold
    print(f"Compared {n_compared} (config, light) pdf pairs across {n_configs} configs.")
    print(f"max relative error = {max_rel_err:.3e} (threshold {threshold:.0e})")
    if worst is not None:
        cfg, j, naive, hoisted = worst
        print(f"worst: config {cfg}, light {j}: naive={naive:.9e}  hoisted={hoisted:.9e}")
    print("PASS" if passed else "FAIL")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
