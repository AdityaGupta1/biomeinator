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

#include <vector>

namespace Util
{

template<typename T> inline uint32_t getVectorSizeBytes(const std::vector<T>& vec)
{
    return vec.size() * sizeof(T);
}

template<typename T> inline uint32_t convertByteSizeToCount(uint32_t sizeBytes)
{
    return sizeBytes / static_cast<uint32_t>(sizeof(T));
}

inline std::wstring to_wstring(const char* str)
{
    return std::wstring(str, str + std::strlen(str));
}

inline uint32_t caclulateDispatchSize(const uint32_t size, const uint32_t threadGroupSize)
{
    return (size + threadGroupSize - 1) / threadGroupSize;
}

} // namespace Util
