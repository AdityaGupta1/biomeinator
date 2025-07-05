#pragma once

#include <string>

class Scene;

namespace GltfLoader
{

void loadGltf(const std::string& filePathStr, ::Scene& scene);

} // namespace GltfLoader
