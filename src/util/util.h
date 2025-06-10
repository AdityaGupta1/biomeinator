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

} // namespace Util
