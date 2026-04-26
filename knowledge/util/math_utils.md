_Last edited: 2026-04-26_

# Math Utilities

`src/util/math.h` — small integer math helpers used throughout the codebase.

## `floorDiv`

C/C++ integer division truncates toward zero, but terrain coordinate math needs floor division (toward negative infinity). This matters for converting negative world-space positions to chunk/region positions — without `floorDiv`, chunk (-1, -1) would map to region (0, 0) instead of region (-1, -1).

## `roundUp`

Power-of-two alignment helper. Used for GPU buffer size alignment (D3D12 requires certain size multiples). Only works with power-of-two multiples (asserted).
