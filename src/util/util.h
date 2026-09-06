// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

#include <cstring>
#include <string>
#include <vector>

namespace Util
{

template<typename T> inline size_t getVectorSizeBytes(const std::vector<T>& vec)
{
    return vec.size() * sizeof(T);
}

template<typename T> inline uint32_t convertByteSizeToCount(size_t sizeBytes)
{
    return static_cast<uint32_t>(sizeBytes / sizeof(T));
}

inline std::wstring to_wstring(const char* str)
{
    return std::wstring(str, str + std::strlen(str));
}

// Narrowing conversion; only for strings known to be ASCII (e.g. adapter names). Not the
// range constructor like to_wstring, since that narrows inside the standard library and
// warns (C4244) in every translation unit including this header
inline std::string to_string(const wchar_t* str)
{
    std::string result;
    for (const wchar_t* c = str; *c != 0; ++c)
    {
        result.push_back(static_cast<char>(*c));
    }
    return result;
}

inline uint32_t calculateDispatchSize(const uint32_t size, const uint32_t threadGroupSize)
{
    return (size + threadGroupSize - 1) / threadGroupSize;
}

// Smallest power of two >= max(floor, target), with both inputs treated as
// pow2-friendly. Used by managers that grow GPU scratch buffers in coarse
// pow2 steps so small topology changes don't reallocate every frame.
inline uint32_t nextPow2AtLeast(uint32_t floor, uint32_t target)
{
    uint32_t cap = floor;
    while (cap < target)
    {
        cap *= 2;
    }
    return cap;
}

} // namespace Util
