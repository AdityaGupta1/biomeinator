/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "logger.h"

#include <cstdio>
#include <chrono>
#include <iomanip>
#include <sstream>
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

static void vlog(FILE* out, const char* level, const char* color, const char* fmt, va_list ap)
{
    if (color)
    {
        std::fputs(color, out);
    }

    std::fprintf(out, "[%s][%s] ", timestamp().c_str(), level);
    std::vfprintf(out, fmt, ap);

    if (color)
    {
        std::fputs(RESET, out);
    }

    std::fputc('\n', out);
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
