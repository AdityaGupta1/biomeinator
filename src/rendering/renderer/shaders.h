// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <span>
#include <string_view>

std::span<const unsigned char> getShader(std::string_view name);
