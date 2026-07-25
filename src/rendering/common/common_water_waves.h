// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

// Displacement wave parameters shared between water_waves.hlsli and the CPU mirror of
// waveHeight() in water_displacer.cpp. See water_waves.hlsli for the wave model description
// and the meaning of each strength/freq/speed triple; the shading-normal noise parameters
// live only there since the CPU never evaluates them.

#define WATER_SWELL_WAVE_COUNT 2
#define WATER_SWELL_STRENGTHS { 0.03f, 0.025f }
#define WATER_SWELL_FREQS { { 0.08f, 0.06f }, { -0.05f, 0.11f } }
#define WATER_SWELL_SPEEDS { 0.4f, 0.5f }

#define WATER_CHOP_WAVE_COUNT 3
#define WATER_CHOP_STRENGTHS { 0.02f, 0.015f, 0.01f }
#define WATER_CHOP_FREQS { { 0.8f, 0.6f }, { -0.5f, 1.3f }, { 1.2f, -0.4f } }
#define WATER_CHOP_SPEEDS { 0.55f, 0.85f, 0.7f }

#define WATER_SINE_CHOP_FREQS { { 0.0167f, 0.01f }, { -0.0067f, 0.02f } }
#define WATER_SINE_CHOP_SPEEDS { 0.1f, 0.13f }
