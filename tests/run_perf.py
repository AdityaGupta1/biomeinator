#!/usr/bin/env python3
"""Runs the perf scenes in tests/perf_scenes.json and compares the resulting JSON reports.

    python tests/run_perf.py run [-f REGEX] [-o OUTDIR] [--exe EXE] [extra Biomeinator args...]
    python tests/run_perf.py show OUTDIR_OR_JSON
    python tests/run_perf.py compare BASELINE_DIR CANDIDATE_DIR

`run` launches Biomeinator.exe once per entry with `--perfOutput=<OUTDIR>/<name>.json` and
prints each report. `compare` prints per-scope median deltas between two output directories
produced by `run`. Extra arguments after `run`'s own options are passed through to every
launch (e.g. `--perfFrames=600`).
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_DIR = Path(__file__).resolve().parent.parent
TESTS_DIR = REPO_DIR / "tests"
DEFAULT_EXE = REPO_DIR / "build" / "RelWithDebInfo" / "Biomeinator.exe"
DEFAULT_OUT_DIR = REPO_DIR / "build" / "perf_output" / "latest"


def load_entries(name_filter):
    with open(TESTS_DIR / "perf_scenes.json") as f:
        entries = json.load(f)["perf"]
    if name_filter:
        entries = [e for e in entries if re.search(name_filter, e["name"])]
    return entries


def entry_args(entry):
    args = list(entry.get("args", []))
    if "scene" in entry:
        args.append(f"--scene={(TESTS_DIR / entry['scene']).as_posix()}")
    elif "world" in entry:
        args.append(f"--world={(TESTS_DIR / entry['world']).as_posix()}")
    else:
        sys.exit(f"perf entry '{entry['name']}' must have 'scene' or 'world'")
    return args


def load_reports(path):
    """Returns {name: report} from a directory of <name>.json files, or a single file."""
    path = Path(path)
    files = [path] if path.is_file() else sorted(path.glob("*.json"))
    reports = {}
    for file in files:
        with open(file) as f:
            reports[file.stem] = json.load(f)
    return reports


def scope_rows(report):
    """Yields (label, stats) rows for the whole frame and each scope, indented by depth."""
    yield "frame", report["gpu"]["frameMs"]
    for scope in report["gpu"]["scopes"]:
        yield "  " * (scope["depth"] + 1) + scope["name"], scope["ms"]


def print_report(name, report):
    meta = report["meta"]
    print(f"\n{name}: {meta['measuredFrames']} frames at {meta['renderWidth']}x{meta['renderHeight']} "
          f"-> {meta['width']}x{meta['height']} on {meta['adapter']}")
    if not meta["stablePowerState"]:
        print("  (stable power state unavailable; timings will be noisier)")
    if meta["timedOut"]:
        print("  (run timed out; results may be incomplete)")
    cpu = report["cpu"]["frameMs"]
    if cpu:
        print(f"  cpu frame: median {cpu['median']:.3f} ms, p95 {cpu['p95']:.3f} ms")
    print(f"  {'gpu scope':<28}{'median':>10}{'mean':>10}{'p95':>10}{'max':>10}{'count':>8}")
    for label, stats in scope_rows(report):
        if stats is None:
            continue
        print(f"  {label:<28}{stats['median']:>10.3f}{stats['mean']:>10.3f}{stats['p95']:>10.3f}"
              f"{stats['max']:>10.3f}{stats['count']:>8}")


def cmd_run(args, passthrough):
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = Path(args.exe)
    if not exe.is_file():
        sys.exit(f"exe not found: {exe}")

    failed = []
    for entry in load_entries(args.filter):
        name = entry["name"]
        output = out_dir / f"{name}.json"
        command = [str(exe), f"--perfOutput={output.as_posix()}"] + entry_args(entry) + passthrough
        print(f"\n=== {name} ===\n{' '.join(command)}", flush=True)
        result = subprocess.run(command, cwd=REPO_DIR)
        if result.returncode != 0 or not output.is_file():
            print(f"{name}: FAILED (exit code {result.returncode})")
            failed.append(name)
            continue
        print_report(name, load_reports(output)[name])

    if failed:
        sys.exit(f"\nfailed: {', '.join(failed)}")


def cmd_show(args):
    for name, report in load_reports(args.path).items():
        print_report(name, report)


def cmd_compare(args):
    baseline = load_reports(args.baseline)
    candidate = load_reports(args.candidate)
    for name in sorted(set(baseline) & set(candidate)):
        base_rows = dict(scope_rows(baseline[name]))
        cand_rows = dict(scope_rows(candidate[name]))
        print(f"\n{name}")
        print(f"  {'gpu scope':<28}{'baseline':>10}{'candidate':>10}{'delta':>10}")
        for label in base_rows:
            base = base_rows[label]
            cand = cand_rows.get(label)
            if base is None or cand is None:
                continue
            delta = (cand["median"] - base["median"]) / base["median"] * 100.0 if base["median"] > 0 else 0.0
            print(f"  {label:<28}{base['median']:>10.3f}{cand['median']:>10.3f}{delta:>+9.1f}%")
        for label in cand_rows.keys() - base_rows.keys():
            print(f"  {label:<28}{'-':>10}{cand_rows[label]['median']:>10.3f}{'new':>10}")
    for name in sorted(set(baseline) ^ set(candidate)):
        print(f"\n{name}: only in {'baseline' if name in baseline else 'candidate'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    run = subparsers.add_parser("run")
    run.add_argument("-f", "--filter", help="regex on entry names")
    run.add_argument("-o", "--out-dir", default=str(DEFAULT_OUT_DIR))
    run.add_argument("--exe", default=str(DEFAULT_EXE))

    show = subparsers.add_parser("show")
    show.add_argument("path")

    compare = subparsers.add_parser("compare")
    compare.add_argument("baseline")
    compare.add_argument("candidate")

    args, passthrough = parser.parse_known_args()
    if args.command == "run":
        cmd_run(args, passthrough)
    elif args.command == "show":
        cmd_show(args)
    else:
        if passthrough:
            parser.error(f"unrecognized arguments: {' '.join(passthrough)}")
        cmd_compare(args)


if __name__ == "__main__":
    main()
