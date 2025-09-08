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

#pragma once

#ifdef _DEBUG
#include <iostream>

#define ASSERT_IMPL(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::cerr << "Assertion failed: (" << #cond << ")\n";                                                      \
            if constexpr (sizeof(msg) > 1)                                                                             \
            {                                                                                                          \
                std::cerr << "Message: " << msg << "\n";                                                               \
            }                                                                                                          \
            std::cerr << "File: " << __FILE__ << ", line: " << __LINE__ << std::endl;                                  \
            __debugbreak();                                                                                            \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT(cond, ...) ASSERT_IMPL(cond, "" __VA_ARGS__)
#else
#define ASSERT(cond, ...) ((void)0)
#endif