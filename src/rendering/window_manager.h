// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include "dxr_includes.h"
#include "host_structs.h"

namespace WindowManager
{

extern HWND hwnd;

void init();

PlayerInput getPlayerInput();

// -1, 0, or +1: direction of fast time scrubbing requested by the bracket keys.
float getTimeScrubDirection();

} // namespace WindowManager
