_Last edited: 2026-10-04_

# Copyright Headers

Every first-party source file starts with an `SPDX-License-Identifier: MIT` line followed by
`Copyright (c) <years> Aditya Gupta`, in whatever comment syntax the file uses. Third-party
files keep their original notice (e.g. `agx.hlsli`), and the repo-root `LICENSE.txt` covers the
whole project, so its range is the project's lifetime rather than any one file's.

## The rule

`<years>` runs from the year the file's content was first written to the year its content was
last changed: `2026` for a file created and only edited in 2026, `2025-2026` for one created in
2025 and edited since, `2025` for one untouched after 2025.

- **Header-only commits don't count**, whether they change the license text or only fix the
  years. The April 2026 GPL-to-MIT switch rewrote every header
  without touching code, and stamped `2025-2026` on all of them regardless of history; that is
  how many 2026-only files ended up with the wrong range.
- **Renames and moves keep the start year.** A file moved to a new folder or renamed is the
  same content. A pure rename (no content change) doesn't bump the end year either.
- **Splits keep the older start year** when a recognisable part of the new file (whole
  functions or declarations) was carried over from an older file, e.g. the pieces of
  `renderer/` split out of the 2025 `renderer.cpp`. A new file that only borrowed a few
  lines of boilerplate from an older one as a template (resource-creation snippets,
  `numthreads`/include preambles) starts at its own creation year. If the old code was
  rewritten before the split, it no longer counts.
- **Year rollover**: the first content edit of a new year extends the end year. Bump it in the
  same change rather than in a later sweep.

## Auditing

`git log --follow` alone is not trustworthy here. The old 15-line GPL block made small files
look over 50% similar to unrelated ones, so `--follow` jumps into other files' histories
(e.g. `logger.h` "copied from" `gltf_loader.h`). Check each rename/copy step by comparing the
two versions with license lines stripped, and use `git blame -C -C -C` on a file's first
version to see how much of it was carried over from older files.
