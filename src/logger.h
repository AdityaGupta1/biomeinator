// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <string>

namespace Logger
{

void log(const char* fmt, ...);

void logWarning(const char* fmt, ...);

void logError(const char* fmt, ...);

} // namespace Logger
