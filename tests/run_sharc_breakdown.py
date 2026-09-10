#!/usr/bin/env python3
"""Separate SHaRC beauty into primary NEE, cache, first-ray emission and remainder.

Render single-sample and accumulated component images with identical settings.
Report linear energy and single-sample squared error against the accumulated image.
The latter estimates noise, not bias against an uncached ground truth.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from run_sharc import ROOT, read_pfm, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/sharc_breakdown")
    parser.add_argument("--scenes", nargs="+", default=["cave_lights", "crystal_caves"])
    parser.add_argument("--frames", type=int, default=256)
    parser.add_argument("--modes", nargs="+", type=int, default=[5, 6, 7, 8, 9, 10, 11])
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = ROOT / "build/RelWithDebInfo/Biomeinator.exe"
    components = {5: "primary_nee", 6: "cache_contribution", 7: "first_ray_emission", 8: "remainder",
                  9: "later_nee", 10: "later_emission", 11: "visible_emission"}
    results = {}
    for scene in args.scenes:
        result = {}
        for mode in args.modes:
            name = components[mode]
            for count in (1, args.frames):
                prefix = out / f"{scene}_{name}_{count}spp"
                print(f"Rendering {prefix.name}", flush=True)
                run(exe, [f"--world={ROOT.as_posix()}/tests/voxel/{scene}",
                    "--width=960", "--height=540", "--antialiasingMode=1",
                    f"--maxAccumulatedFrames={count}", "--maxPathDepth=16",
                    "--rngSeed=1738", "--frameGeneration=false", "--doPathSplitting=false",
                    "--noJitter", "--sharc=true", "--sharcWarmupFrames=256",
                    f"--sharcDebug={mode}", f"--testOutput={prefix.as_posix()}.png",
                    f"--testRadianceOutput={prefix.as_posix()}.pfm"], prefix.with_suffix(".log"), 240)
            single = read_pfm(out / f"{scene}_{name}_1spp.pfm")
            mean = read_pfm(out / f"{scene}_{name}_{args.frames}spp.pfm")
            if not np.isfinite(single).all() or not np.isfinite(mean).all():
                raise RuntimeError(f"Nonfinite component: {scene}/{name}")
            result[name] = {"mean_radiance": float(mean.mean()),
                            "single_sample_mse": float(np.mean((single - mean) ** 2))}
        energy = sum(v["mean_radiance"] for v in result.values())
        error = sum(v["single_sample_mse"] for v in result.values())
        for value in result.values():
            value["energy_share"] = value["mean_radiance"] / max(energy, 1e-10)
            value["component_mse_share"] = value["single_sample_mse"] / max(error, 1e-10)
        results[scene] = result
        (out / "metrics.json").write_text(json.dumps(results, indent=2))
        print(json.dumps({scene: result}, indent=2), flush=True)


if __name__ == "__main__":
    main()
