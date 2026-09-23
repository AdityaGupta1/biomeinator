_Last edited: 2026-09-22_

# CPU Unit Tests

`BiomeinatorUnitTests` is the fast, CPU-only Catch2 target. It is deliberately independent of
`Biomeinator` and the golden-image runner: changing a math helper or terrain rule should be
testable without linking the renderer, starting a D3D12 device, or producing shader output.
CTest discovers each Catch2 case separately and gives every discovered case the `unit` label,
so a single failure is visible by name and the suite can be selected without running GPU tests.

The initial coverage protects deterministic, widely reused building blocks: integer boundary
math, circular-buffer wrap/reset state, procedural RNG sequences and ranges, Halton generation,
CPU/GPU-shared vertex packing, and block face/orientation rules. Fixed expected RNG and packing
values are compatibility checks, not statistical tests: world generation and shader decoding
depend on those bit-level results remaining stable.

Build and run the suite with:

```powershell
cmake --build build --config RelWithDebInfo --target BiomeinatorUnitTests
ctest --test-dir build -C RelWithDebInfo -L unit --output-on-failure
```

Catch2 is pinned as the `external/Catch2` submodule. `include(CTest)` supplies the conventional
`BUILD_TESTING` option; configuring with `-DBUILD_TESTING=OFF` omits Catch2 and the unit target.
The engine's `ASSERT` checks follow the same configuration policy in the unit target as in the
renderer, while test assertions are always provided by Catch2.

Keep D3D allocation/copy tests and full rendered-image comparisons out of this target. Those
have different prerequisites and failure modes and should remain separately selectable through
CTest labels.
