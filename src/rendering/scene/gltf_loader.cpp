#include "gltf_loader.h"

#include "tinygltf/tiny_gltf.h"

#include "scene.h"

using namespace tinygltf;

namespace GltfLoader
{

void loadGltf(const std::string& filePath, ::Scene& scene)
{
    printf("Loading GLTF file: %s\n", filePath.c_str());
}

} // namespace GLtfLoader
