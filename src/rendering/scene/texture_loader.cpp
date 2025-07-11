#include "texture_loader.h"

#include <algorithm>
#include <cmath>
#include "stb/stb_image.h"

namespace TextureLoader
{

bool loadFromFile(const std::string& path, std::vector<uint8_t>& outData, uint32_t& width, uint32_t& height)
{
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!data)
    {
        return false;
    }

    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    const size_t sizeBytes = static_cast<size_t>(width) * height * 4;
    outData.resize(sizeBytes);
    memcpy(outData.data(), data, sizeBytes);

    stbi_image_free(data);
    return true;
}

} // namespace TextureLoader
