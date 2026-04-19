// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#pragma once

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

inline uint32_t calculateDispatchSize(const uint32_t size, const uint32_t threadGroupSize)
{
    return (size + threadGroupSize - 1) / threadGroupSize;
}

} // namespace Util
