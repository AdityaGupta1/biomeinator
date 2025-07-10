#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace TextureLoader
{

bool loadFromFile(const std::string& path, std::vector<uint8_t>& outData, uint32_t& width, uint32_t& height);

} // namespace TextureLoader
