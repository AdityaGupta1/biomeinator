#pragma once

#include <string>

class Scene;

namespace GltfLoader
{

void loadGltf(const std::string& filePath, ::Scene& scene);

} // namespace GltfLoader
