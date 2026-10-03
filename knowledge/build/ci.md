_Last edited: 2026-10-03_

# Continuous Integration

`.github/workflows/build.yml` builds every configured target in RelWithDebInfo on a hosted
Windows runner and runs the `unit`-labelled CTest cases. The runner has no GPU, so it only
proves that the code and shaders compile; rendering tests and goldens must still be run locally.
Building all targets (not just `Biomeinator`) means the rendering test runner and unit test
binary also have to compile.

The checkout is deliberately narrower than a developer clone, to keep CI free and fast:

- **Only `external/` submodules are fetched.** `reference/DirectX-Specs` is documentation and is
  never a build input (see [dependencies.md](dependencies.md)).
- **Only the LFS objects needed to compile and link are pulled, and they are cached.** LFS
  downloads count against the account's monthly LFS bandwidth quota, the one part of CI that can
  cost money. That means import libraries plus dxc's DLLs (dxc runs during the build), about
  40 MB, instead of all of `external/` (over 500 MB, mostly DLSS DLLs, docs and symbols).
  Everything else stays a pointer file: the runtime DLL and asset copy steps copy pointers
  without complaint, and the unit tests load none of them. A new prebuilt SDK whose `.lib` or
  build-time tool lives outside that pattern must be added to `LFS_BUILD_INPUTS`. The cache key
  is derived from the matching LFS object IDs, so updating a vendored SDK invalidates it.
