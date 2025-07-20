/*
This file is mostly AI-generated and exists solely to load test scenes for verifying path tracing results. It works only
on a specific subset of glTF files and is not guaranteed to work for files outside that subset.
*/

#include "gltf_loader.h"

#include "tinygltf/tiny_gltf.h"

#include <filesystem>
#include <string>

#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"
#include "scene.h"

using namespace tinygltf;

namespace GltfLoader
{

void loadGltf(const std::string& filePathStr, ::Scene& scene)
{
    printf("Loading GLTF file: %s\n", filePathStr.c_str());

    scene.clear();

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    const bool isGlb = std::filesystem::path(filePathStr).extension() == ".glb";
    const bool loaded = isGlb ? loader.LoadBinaryFromFile(&model, &err, &warn, filePathStr)
                              : loader.LoadASCIIFromFile(&model, &err, &warn, filePathStr);

    if (!warn.empty())
    {
        printf("glTF warning: %s\n", warn.c_str());
    }
    if (!err.empty())
    {
        printf("glTF error: %s\n", err.c_str());
    }
    if (!loaded)
    {
        printf("Failed to load glTF file\n");
        return;
    }

    std::vector<uint32_t> textureIds;
    textureIds.reserve(model.images.size());
    for (tinygltf::Image& image : model.images)
    {
        textureIds.push_back(scene.addTexture(std::move(image.image), image.width, image.height));
    }

    ToFreeList toFreeList;

    std::vector<uint32_t> materialIds;
    materialIds.reserve(model.materials.size());
    std::vector<bool> materialIsEmissive;
    materialIsEmissive.reserve(model.materials.size());
    for (const tinygltf::Material& gltfMat : model.materials)
    {
        ::Material material;

        if (gltfMat.pbrMetallicRoughness.baseColorFactor.size() >= 3)
        {
            material.diffuseColor = {
                static_cast<float>(gltfMat.pbrMetallicRoughness.baseColorFactor[0]),
                static_cast<float>(gltfMat.pbrMetallicRoughness.baseColorFactor[1]),
                static_cast<float>(gltfMat.pbrMetallicRoughness.baseColorFactor[2]),
            };

            if (material.diffuseColor.x == 0 && material.diffuseColor.y == 0 && material.diffuseColor.z == 0)
            {
                material.diffuseWeight = 0.f;
            }
        }

        if (gltfMat.emissiveFactor.size() == 3)
        {
            material.emissiveColor = {
                static_cast<float>(gltfMat.emissiveFactor[0]),
                static_cast<float>(gltfMat.emissiveFactor[1]),
                static_cast<float>(gltfMat.emissiveFactor[2]),
            };
            const bool anyEmissive = material.emissiveColor.x != 0 || material.emissiveColor.y != 0
                                     || material.emissiveColor.z != 0;
            material.emissiveStrength = anyEmissive ? 1.f : 0.f;
        }

        const auto emissiveExtIt = gltfMat.extensions.find("KHR_materials_emissive_strength");
        if (emissiveExtIt != gltfMat.extensions.end())
        {
            const tinygltf::Value& ext = emissiveExtIt->second;
            if (ext.IsObject() && ext.Has("emissiveStrength"))
            {
                const tinygltf::Value& val = ext.Get("emissiveStrength");
                if (val.IsNumber())
                {
                    material.emissiveStrength = static_cast<float>(val.GetNumberAsDouble());
                }
            }
        }

        if (gltfMat.pbrMetallicRoughness.baseColorTexture.index >= 0)
        {
            const int texIdx = gltfMat.pbrMetallicRoughness.baseColorTexture.index;
            if (texIdx < model.textures.size())
            {
                const int imgIdx = model.textures[texIdx].source;
                if (imgIdx >= 0 && imgIdx < textureIds.size())
                {
                    material.diffuseTextureId = textureIds[imgIdx];
                }
            }
        }

        const uint32_t id = scene.addMaterial(toFreeList, &material);
        materialIds.push_back(id);
        materialIsEmissive.push_back(material.emissiveStrength > 0.f);
    }

    const auto readAccessorData = [&](const tinygltf::Accessor& accessor) {
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        const tinygltf::Buffer& buffer = model.buffers[view.buffer];
        return buffer.data.data() + view.byteOffset + accessor.byteOffset;
    };

    const auto getStride = [&](const tinygltf::Accessor& accessor) {
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        if (view.byteStride != 0)
        {
            return static_cast<size_t>(view.byteStride);
        }

        size_t componentSize = 0;
        switch (accessor.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            componentSize = 4;
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            componentSize = 2;
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            componentSize = 4;
            break;
        default:
            componentSize = 4;
            break;
        }

        int numComponents = 1;
        switch (accessor.type)
        {
        case TINYGLTF_TYPE_VEC2:
            numComponents = 2;
            break;
        case TINYGLTF_TYPE_VEC3:
            numComponents = 3;
            break;
        case TINYGLTF_TYPE_VEC4:
            numComponents = 4;
            break;
        default:
            break;
        }

        return componentSize * static_cast<size_t>(numComponents);
    };

    for (const Node& node : model.nodes)
    {
        if (node.mesh < 0)
        {
            continue;
        }

        DirectX::XMMATRIX transform = DirectX::XMMatrixIdentity();
        if (node.matrix.size() == 16)
        {
            float nodeMatrixValues[16];
            for (int i = 0; i < 16; ++i)
            {
                nodeMatrixValues[i] = static_cast<float>(node.matrix[i]);
            }
            transform = DirectX::XMMATRIX(nodeMatrixValues);
        }
        else
        {
            if (node.scale.size() == 3)
            {
                transform *= DirectX::XMMatrixScaling(static_cast<float>(node.scale[0]),
                                                      static_cast<float>(node.scale[1]),
                                                      static_cast<float>(node.scale[2]));
            }

            if (node.rotation.size() == 4)
            {
                const DirectX::XMVECTOR quat = DirectX::XMVectorSet(static_cast<float>(node.rotation[0]),
                                                                    static_cast<float>(node.rotation[1]),
                                                                    static_cast<float>(node.rotation[2]),
                                                                    static_cast<float>(node.rotation[3]));
                transform *= DirectX::XMMatrixRotationQuaternion(quat);
            }

            if (node.translation.size() == 3)
            {
                transform *= DirectX::XMMatrixTranslation(static_cast<float>(node.translation[0]),
                                                          static_cast<float>(node.translation[1]),
                                                          static_cast<float>(node.translation[2]));
            }
        }

        const Mesh& mesh = model.meshes[node.mesh];
        for (const Primitive& prim : mesh.primitives)
        {
            Instance* instance = scene.requestNewInstance(toFreeList);

            uint32_t matId = MATERIAL_ID_INVALID;
            if (prim.material >= 0 && static_cast<size_t>(prim.material) < materialIds.size())
            {
                matId = materialIds[prim.material];
            }
            instance->setMaterialId(matId);

            const Accessor& posAccessor = model.accessors[prim.attributes.find("POSITION")->second];
            const Accessor& norAccessor = model.accessors[prim.attributes.find("NORMAL")->second];
            const Accessor* uvAccessor = nullptr;
            const auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end())
            {
                uvAccessor = &model.accessors[uvIt->second];
            }

            const size_t vertCount = posAccessor.count;
            instance->host_verts.resize(vertCount);

            const unsigned char* posData = readAccessorData(posAccessor);
            const unsigned char* norData = readAccessorData(norAccessor);
            const unsigned char* uvData = uvAccessor ? readAccessorData(*uvAccessor) : nullptr;

            const size_t posStride = getStride(posAccessor);
            const size_t norStride = getStride(norAccessor);
            const size_t uvStride = uvAccessor ? getStride(*uvAccessor) : 0;

            for (size_t v = 0; v < vertCount; ++v)
            {
                const float* p = reinterpret_cast<const float*>(posData + posStride * v);
                const float* n = reinterpret_cast<const float*>(norData + norStride * v);

                DirectX::XMFLOAT2 uv = { 0.f, 0.f };
                if (uvAccessor)
                {
                    const float* uvf = reinterpret_cast<const float*>(uvData + uvStride * v);
                    uv = { uvf[0], uvf[1] };
                }

                instance->host_verts[v] = { { p[0], p[1], p[2] }, { n[0], n[1], n[2] }, uv };
            }

            if (prim.indices >= 0)
            {
                const Accessor& idxAccessor = model.accessors[prim.indices];
                const unsigned char* idxData = readAccessorData(idxAccessor);
                const size_t idxCount = idxAccessor.count;
                instance->host_idxs.resize(idxCount);

                const size_t idxStride = getStride(idxAccessor);

                for (size_t i = 0; i < idxCount; ++i)
                {
                    uint32_t idx = 0;
                    switch (idxAccessor.componentType)
                    {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        idx = *(reinterpret_cast<const uint8_t*>(idxData + idxStride * i));
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        idx = *(reinterpret_cast<const uint16_t*>(idxData + idxStride * i));
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        idx = *(reinterpret_cast<const uint32_t*>(idxData + idxStride * i));
                        break;
                    default:
                        break;
                    }
                    instance->host_idxs[i] = idx;
                }
            }

            const bool isEmissive = prim.material >= 0 &&
                                    static_cast<uint32_t>(prim.material) < materialIsEmissive.size() &&
                                    materialIsEmissive[prim.material];
            if (isEmissive)
            {
                const uint32_t triCount =
                    instance->host_idxs.empty() ? instance->host_verts.size() / 3 : instance->host_idxs.size() / 3;
                instance->host_areaLights.resize(triCount);
                for (uint32_t triIdx = 0; triIdx < triCount; ++triIdx)
                {
                    uint32_t i0 = triIdx * 3;
                    uint32_t i1 = i0 + 1;
                    uint32_t i2 = i0 + 2;
                    if (!instance->host_idxs.empty())
                    {
                        i0 = instance->host_idxs[i0];
                        i1 = instance->host_idxs[i1];
                        i2 = instance->host_idxs[i2];
                    }

                    AreaLight& light = instance->host_areaLights[triIdx];
                    light.pos0 = instance->host_verts[i0].pos;
                    light.pos1 = instance->host_verts[i1].pos;
                    light.pos2 = instance->host_verts[i2].pos;
                    light.instanceId = instance->getId();
                    light.triangleIdx = triIdx;
                }
            }

            DirectX::XMStoreFloat3x4(&instance->transform, transform);

            scene.markInstanceReadyForBlasBuild(instance);
        }
    }

    toFreeList.freeAll();
}

} // namespace GltfLoader
