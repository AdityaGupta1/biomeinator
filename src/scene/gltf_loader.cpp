// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

/*
This file is mostly AI-generated and exists solely to load test scenes for verifying path tracing results. It works
on a specific subset of glTF files and is not guaranteed to work for files outside that subset.

For example, exporting a Blender scene with a Glossy BSDF as a glTF does not preserve the glossy color. For scenes with
colored glossy reflection, I manually added the color to the respective glTF files.
*/

#include "gltf_loader.h"

#include <tiny_gltf.h>

#include <filesystem>
#include <string>
#include <stdexcept>

#include "rendering/buffer/to_free_list.h"
#include "rendering/common/common_structs.h"
#include "scene.h"

#include "logger.h"

using namespace tinygltf;

namespace GltfLoader
{

void loadGltf(const std::string& filePathStr, ::Scene& scene)
{
    Logger::log("Loading GLTF file: %s", std::filesystem::path(filePathStr).generic_string().c_str());

    scene.reset();
    scene.init();

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    const bool isGlb = std::filesystem::path(filePathStr).extension() == ".glb";
    const bool loaded = isGlb ? loader.LoadBinaryFromFile(&model, &err, &warn, filePathStr)
                              : loader.LoadASCIIFromFile(&model, &err, &warn, filePathStr);

    if (!warn.empty())
    {
        Logger::logWarning("glTF warning: %s\n", warn.c_str());
    }
    if (!err.empty())
    {
        Logger::logError("glTF error: %s\n", err.c_str());
    }
    if (!loaded)
    {
        throw std::runtime_error("Failed to load glTF file");
    }

    std::vector<uint32_t> textureIds;
    textureIds.reserve(model.images.size());
    for (tinygltf::Image& image : model.images)
    {
        textureIds.push_back(scene.addTexture(std::move(image.image), image.width, image.height));
    }

    ToFreeList toFreeList;

    std::vector<uint32_t> materialIdxs;
    materialIdxs.reserve(model.materials.size());
    std::vector<bool> materialIsEmissive;
    materialIsEmissive.reserve(model.materials.size());
    for (const tinygltf::Material& gltfMat : model.materials)
    {
        ::Material material;

        if (gltfMat.emissiveFactor.size() == 3)
        {
            material.emissiveColor = {
                static_cast<float>(gltfMat.emissiveFactor[0]),
                static_cast<float>(gltfMat.emissiveFactor[1]),
                static_cast<float>(gltfMat.emissiveFactor[2]),
            };
        }
        const bool hasEmissiveColor =
            (material.emissiveColor.x != 0 || material.emissiveColor.y != 0 || material.emissiveColor.z != 0);

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
        else if (hasEmissiveColor)
        {
            material.emissiveStrength = 1.f;
        }

        if (gltfMat.emissiveTexture.index >= 0)
        {
            const int texIdx = gltfMat.emissiveTexture.index;

            if (texIdx < model.textures.size())
            {
                const int imgIdx = model.textures[texIdx].source;

                if (imgIdx >= 0 && imgIdx < textureIds.size())
                {
                    material.emissiveColorTextureId = textureIds[imgIdx];
                }
            }
        }

        const bool hasEmission = material.emissiveStrength > 0;

        bool hasDiffuse, hasGlossyReflection, hasGlossyTransmission;

        if (hasEmission)
        {
            hasDiffuse = false;
            hasGlossyReflection = false;
            hasGlossyTransmission = false;
        }
        else
        {
            hasDiffuse = false;
            hasGlossyReflection = true;
            hasGlossyTransmission = false;

            const auto& pbr = gltfMat.pbrMetallicRoughness;
            // This is a super scuffed way of determining whether the material has the pbrMetallicRoughness struct.
            // Ideally, I would use some JSON utils to check this for real. But this works for now.
            const bool hasPbr = !(pbr.metallicFactor == 1.0 && pbr.roughnessFactor == 1.0);
            if (hasPbr)
            {
                // Use metallicFactor to determine if material is metallic (specular-only) or dielectric (can have diffuse)
                // metallicFactor == 1.0 (default) = metallic/specular only
                // metallicFactor == 0 = dielectric, can have diffuse
                const bool isMetallic = pbr.metallicFactor >= 1.0;

                if (isMetallic)
                {
                    // Metallic material: specular only, no diffuse
                    hasDiffuse = false;
                }
                else
                {
                    // Dielectric material: can have diffuse
                    if (gltfMat.pbrMetallicRoughness.baseColorTexture.index >= 0)
                    {
                        const int texIdx = gltfMat.pbrMetallicRoughness.baseColorTexture.index;

                        if (texIdx < model.textures.size())
                        {
                            const int imgIdx = model.textures[texIdx].source;

                            if (imgIdx >= 0 && imgIdx < textureIds.size())

                            {
                                material.baseColorTextureId = textureIds[imgIdx];
                                hasDiffuse = true;
                            }
                        }
                    }
                    else
                    {
                        material.baseColor = {
                            static_cast<float>(pbr.baseColorFactor[0]),
                            static_cast<float>(pbr.baseColorFactor[1]),
                            static_cast<float>(pbr.baseColorFactor[2]),
                        };

                        hasDiffuse = !(material.baseColor.x == 0 && material.baseColor.y == 0 && material.baseColor.z == 0);
                    }
                }
            }

            const auto specularExtIt = gltfMat.extensions.find("KHR_materials_specular");
            if (specularExtIt != gltfMat.extensions.end())
            {
                const tinygltf::Value& ext = specularExtIt->second;
                if (ext.IsObject())
                {
                    if (ext.Has("specularFactor"))
                    {
                        const tinygltf::Value& val = ext.Get("specularFactor");
                        if (val.IsNumber())
                        {
                            const double specularFactor = val.GetNumberAsDouble();
                            // For metallic materials (specular-only), always allow specular reflection
                            // even if specularFactor is 0 (it might just mean no specular color tint)
                            // For dielectric materials, respect specularFactor
                            if (!hasDiffuse)
                            {
                                // Metallic/specular-only material: keep specular enabled
                                hasGlossyReflection = true;
                            }
                            else
                            {
                                // Dielectric material with diffuse: respect specularFactor
                                hasGlossyReflection = specularFactor != 0.0;
                            }
                        }
                    }

                    if (hasGlossyReflection && ext.Has("specularColorFactor"))
                    {
                        const tinygltf::Value& val = ext.Get("specularColorFactor");
                        if (val.IsArray() && val.ArrayLen() >= 3)
                        {
                            material.glossyReflectionTint = {
                                static_cast<float>(val.Get(0).GetNumberAsDouble()),
                                static_cast<float>(val.Get(1).GetNumberAsDouble()),
                                static_cast<float>(val.Get(2).GetNumberAsDouble()),
                            };
                        }
                    }
                }
            }

            const auto transmissionExtIt = gltfMat.extensions.find("KHR_materials_transmission");
            if (transmissionExtIt != gltfMat.extensions.end())
            {
                const tinygltf::Value& ext = transmissionExtIt->second;
                if (ext.IsObject() && ext.Has("transmissionFactor"))
                {
                    const tinygltf::Value& val = ext.Get("transmissionFactor");
                    if (val.IsNumber())
                    {
                        const double transmissionFactor = val.GetNumberAsDouble();
                        if (transmissionFactor > 0.0)
                        {
                            hasGlossyTransmission = true;
                        }
                    }
                }
            }

            const auto iorExtIt = gltfMat.extensions.find("KHR_materials_ior");
            if (iorExtIt != gltfMat.extensions.end())
            {
                const tinygltf::Value& ext = iorExtIt->second;
                if (ext.IsObject() && ext.Has("ior"))
                {
                    const tinygltf::Value& val = ext.Get("ior");
                    if (val.IsNumber())
                    {
                        const double ior = val.GetNumberAsDouble();
                        if (ior > 0.0)
                        {
                            material.ior = static_cast<float>(ior);
                        }
                    }
                }
            }

            if (hasGlossyTransmission)
            {
                hasDiffuse = false;
            }
        }

        material.setHasDiffuse(hasDiffuse);
        material.setHasGlossyReflection(hasGlossyReflection);
        material.setHasGlossyTransmission(hasGlossyTransmission);

        const uint32_t id = scene.addMaterial(toFreeList, &material);
        materialIdxs.push_back(id);
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

            uint32_t materialIdx = MATERIAL_IDX_INVALID;
            if (prim.material >= 0 && static_cast<size_t>(prim.material) < materialIdxs.size())
            {
                materialIdx = materialIdxs[prim.material];
            }
            instance->setMaterialIdx(materialIdx);

            const Accessor& posAccessor = model.accessors[prim.attributes.find("POSITION")->second];
            const Accessor& norAccessor = model.accessors[prim.attributes.find("NORMAL")->second];
            const Accessor* uvAccessor = nullptr;
            const auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt != prim.attributes.end())
            {
                uvAccessor = &model.accessors[uvIt->second];
            }

            const size_t vertCount = posAccessor.count;
            std::vector<Vertex>& host_verts = instance->host_verts;
            host_verts.resize(vertCount);

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

                host_verts[v] = { { p[0], p[1], p[2] }, { n[0], n[1], n[2] }, uv };

                DirectX::XMFLOAT3 pos_WS;
                DirectX::XMStoreFloat3(
                    &pos_WS,
                    DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&host_verts[v].pos_OS), transform));
                scene.expandBounds({ pos_WS.x, pos_WS.y, pos_WS.z });
            }

            std::vector<uint32_t>& host_idxs = instance->host_idxs;
            if (prim.indices >= 0)
            {
                const Accessor& idxAccessor = model.accessors[prim.indices];
                const unsigned char* idxData = readAccessorData(idxAccessor);
                const size_t idxCount = idxAccessor.count;
                host_idxs.resize(idxCount);

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
                    host_idxs[i] = idx;
                }
            }

            DirectX::XMFLOAT3X4 instanceTransform;
            DirectX::XMStoreFloat3x4(&instanceTransform, transform);
            instance->setTransform(instanceTransform);

            instance->host_perTriDatas.resize(instance->getTriCount());
            instance->finalizeGeometry();

            const bool isEmissive = prim.material >= 0 &&
                                    static_cast<uint32_t>(prim.material) < materialIsEmissive.size() &&
                                    materialIsEmissive[prim.material];
            if (isEmissive)
            {
                const uint32_t triCount = instance->getTriCount();
                std::vector<uint32_t> triangleIdxs;
                triangleIdxs.reserve(triCount);
                for (uint32_t triIdx = 0; triIdx < triCount; ++triIdx)
                {
                    triangleIdxs.push_back(triIdx);
                }
                instance->addAreaLights(triangleIdxs);
            }

            scene.markInstanceReadyForBlasBuild(instance);
        }
    }

    toFreeList.freeAll();
}

} // namespace GltfLoader
