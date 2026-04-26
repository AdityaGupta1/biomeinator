_Last edited: 2026-04-26_

# Ring Buffer

`src/util/ring_buffer.h` — fixed-capacity circular buffer template. Currently used for frame time history (600 samples in the renderer).

## Access Pattern

No random access or iteration helpers — consumers get the raw `std::array` plus `offset` and `size`, and are expected to handle the wraparound themselves. This is intentional: the primary consumer (ImGui frame time graph) needs the raw array anyway.
