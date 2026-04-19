import re
from pathlib import Path

GPL_PATTERN = re.compile(
    r"/\*\s*\n"
    r"Biomeinator - real-time path traced voxel engine\s*\n"
    r"Copyright \(C\) (?:2025|2026) Aditya Gupta\s*\n"
    r"\s*\n"
    r"This program is free software.*?\n"
    r"along with this program\..*?\n"
    r"\*/\s*\n",
    re.DOTALL,
)

MIT_HEADER = "// SPDX-License-Identifier: MIT\n// Copyright (c) 2025-2026 Aditya Gupta\n\n"

EXTENSIONS = {".cpp", ".h", ".hlsl", ".hlsli"}

src_dir = Path("src")
count = 0

for path in sorted(src_dir.rglob("*")):
    if path.suffix not in EXTENSIONS:
        continue

    text = path.read_text(encoding="utf-8")
    new_text = GPL_PATTERN.sub(MIT_HEADER, text, count=1)

    if new_text != text:
        path.write_text(new_text, encoding="utf-8")
        count += 1
        print(f"Updated: {path}")

print(f"\n{count} files updated.")
