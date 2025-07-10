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
    outData.resize(static_cast<size_t>(width) * height * 4);

    const size_t numPixels = static_cast<size_t>(width) * height;
    const auto toLinear = [](float c) {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    for (size_t i = 0; i < numPixels; ++i)
    {
        const size_t idx = i * 4;
        for (size_t c = 0; c < 3; ++c)
        {
            const float srgb = data[idx + c] / 255.0f;
            const float linear = std::clamp(toLinear(srgb), 0.0f, 1.0f);
            outData[idx + c] = static_cast<uint8_t>(linear * 255.0f + 0.5f);
        }

        outData[idx + 3] = data[idx + 3];
    }

    stbi_image_free(data);
    return true;
}

} // namespace TextureLoader
