#!/usr/bin/env python3
"""SHARC GPU self-test and linear-radiance comparisons against existing scenes.

Requires numpy. Outputs PNG previews, lossless RGB PFM images, logs and metrics.
Example: python tests/run_sharc.py --scenes cornell_box_rtsl evil_room
Performance remains in run_perf.py; use --sharc=true/false with matched arguments.
"""
import argparse
import json
import subprocess
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]

def read_pfm(path):
    with path.open("rb") as file:
        assert file.readline().strip() == b"PF"
        w, h = map(int, file.readline().split())
        scale = float(file.readline())
        return np.fromfile(file, dtype="<f4" if scale < 0 else ">f4").reshape(h, w, 3)[::-1].copy()

def run(exe, args, log, timeout):
    with log.open("w") as output:
        output.write("Command: " + subprocess.list2cmdline([str(exe), *args]) + "\n")
        output.flush()
        result = subprocess.run([str(exe), *args], cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}; see {log}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "build/RelWithDebInfo/Biomeinator.exe")
    parser.add_argument("--out", type=Path, default=ROOT / "build/sharc_validation")
    parser.add_argument("--scenes", nargs="+", default=["cornell_box_rtsl", "evil_room", "diffuse_and_emission_blender_no_tonemap", "water_reflection", "cave_lights"])
    parser.add_argument("--scene-file", type=Path, help="Override scene list with a temporary glTF scene")
    parser.add_argument("--extra", nargs=argparse.REMAINDER, default=[])
    parser.add_argument("--frames", type=int, default=1024)
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=270)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--skip-self-test", action="store_true")
    parser.add_argument("--single-spp", action="store_true", help="Render 1-spp noise comparisons with a 256-frame warmed cache; no bias threshold")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = args.exe.resolve()
    if not args.skip_self_test:
        run(exe, ["--sharcSelfTest", "--width=64", "--height=64", "--sharcCapacityLog2=16", "--frameGeneration=false"], out / "self_test.log", args.timeout)
        if "self-test: PASS" not in (out / "self_test.log").read_text():
            raise RuntimeError("GPU self-test did not report PASS")
        print("GPU self-test: PASS", flush=True)
    entries = {e["name"]: e for e in json.loads((ROOT / "tests/tests.json").read_text())["tests"]}
    entries["crystal_caves"] = {"name": "crystal_caves", "world": "voxel/crystal_caves"}
    if args.scene_file:
        name = args.scene_file.stem
        entries[name] = {"name": name, "scene": str(args.scene_file.resolve())}
        args.scenes = [name]
    results = {}
    for name in args.scenes:
        entry = entries[name]
        scene = [f"--{key}={(ROOT / 'tests' / entry[key]).as_posix()}" for key in ("scene", "world") if key in entry]
        # Keep scene-specific animation/material settings; override sampling/outputs below.
        overrides = {"width", "height", "antialiasingMode", "maxAccumulatedFrames", "tonemapping", "maxPathDepth", "debugView"}
        options = [a for a in entry.get("args", []) if a.lstrip("-").split("=")[0] not in overrides]
        options += [f"--width={args.width}", f"--height={args.height}", "--antialiasingMode=1",
                    f"--maxAccumulatedFrames={1 if args.single_spp else args.frames}", "--maxPathDepth=16", "--rngSeed=1738", "--frameGeneration=false"]
        if args.single_spp:
            options = [a for a in options if not a.startswith("--doPathSplitting=")]
            options += ["--doPathSplitting=false", "--noJitter", "--sharcWarmupFrames=256"]
        for variant in ("reference", "sharc"):
            prefix = out / f"{name}_{variant}"
            print(f"Rendering {name}: {variant}", flush=True)
            run(exe, scene + options + args.extra + [f"--sharc={'true' if variant == 'sharc' else 'false'}",
                "--sharcDiagnostics=true", f"--testOutput={prefix.as_posix()}.png",
                f"--testRadianceOutput={prefix.as_posix()}.pfm"], prefix.with_suffix(".log"), args.timeout)
        reference = read_pfm(out / f"{name}_reference.pfm")
        candidate = read_pfm(out / f"{name}_sharc.pfm")
        if not np.isfinite(candidate).all() or np.min(candidate) < -1e-5:
            raise RuntimeError(f"{name}: invalid radiance")
        diff = candidate - reference
        energy = max(float(reference.mean()), 1e-6)
        metrics = {"reference_mean": float(reference.mean()), "sharc_mean": float(candidate.mean()),
                   "relative_energy_error": float(diff.mean()) / energy,
                   "relative_mae": float(np.abs(diff).mean()) / energy,
                   "rmse": float(np.sqrt(np.mean(diff ** 2)))}
        # Report a separate metric excluding bright pixels, which otherwise dominate energy.
        mask = reference.max(axis=2) < 2
        if mask.any():
            metrics["nonbright_relative_energy_error"] = float(diff[mask].mean()) / max(float(reference[mask].mean()), 1e-6)
        results[name] = metrics
        print(json.dumps({name: metrics}, indent=2), flush=True)
        (out / "metrics.json").write_text(json.dumps(results, indent=2))
    # Thresholds deliberately concern systematic bias; spatial cache error is not pixel-exact.
    failed = [n for n, m in results.items() if abs(m["relative_energy_error"]) > 0.10 or
              abs(m.get("nonbright_relative_energy_error", 0)) > 0.10]
    if failed and not args.single_spp:
        raise SystemExit("Radiance comparison needs investigation: " + ", ".join(failed))

if __name__ == "__main__":
    main()
