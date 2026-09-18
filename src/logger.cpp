// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "logger.h"

#include <cstdio>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstdarg>

#define RESET "\033[0m"
#define RED "\033[38;2;227;38;38m"
#define YELLOW "\033[38;5;229m"

namespace Logger
{

static std::string timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto nowTime = system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&nowTime), "%H:%M:%S");
    return oss.str();
}

// The whole line is assembled before it is written so lines logged from different threads
// (e.g. the pipeline creation workers) do not interleave
static void vlog(FILE* out, const char* level, const char* color, const char* fmt, va_list ap)
{
    std::string line;
    if (color)
    {
        line += color;
    }

    line += "[" + timestamp() + "][" + level + "] ";

    va_list apCopy;
    va_copy(apCopy, ap);
    const int messageLength = std::vsnprintf(nullptr, 0, fmt, apCopy);
    va_end(apCopy);
    if (messageLength > 0)
    {
        const size_t prefixLength = line.size();
        line.resize(prefixLength + messageLength);
        std::vsnprintf(line.data() + prefixLength, messageLength + 1, fmt, ap);
    }

    if (color)
    {
        line += RESET;
    }

    line += '\n';
    std::fputs(line.c_str(), out);
    std::fflush(out);
}

void log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, "info", nullptr, fmt, ap);
    va_end(ap);
}

void logWarning(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, "WARNING", YELLOW, fmt, ap);
    va_end(ap);
}

void logError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, "ERROR", RED, fmt, ap);
    va_end(ap);
}

} // namespace Logger
