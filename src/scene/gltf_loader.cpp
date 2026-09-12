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
#include "util/packing.h"

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

    // Retain decoded bytes only until every required color-space upload has consumed them.
    constexpr uint8_t colorUsage = 1, dataUsage = 2;
    std::vector<uint8_t> imageUsage(model.images.size(), 0);
    const auto markImageUsage = [&](int textureIdx, uint8_t usage) {
        if (textureIdx < 0 || static_cast<size_t>(textureIdx) >= model.textures.size())
        {
            return;
        }
        const int imageIdx = model.textures[textureIdx].source;
        if (imageIdx >= 0 && static_cast<size_t>(imageIdx) < model.images.size())
        {
            imageUsage[imageIdx] |= usage;
        }
    };
    for (const auto& material : model.materials)
    {
        markImageUsage(material.emissiveTexture.index, colorUsage);
        if (material.pbrMetallicRoughness.metallicFactor < 1.0)
        {
            markImageUsage(material.pbrMetallicRoughness.baseColorTexture.index, colorUsage);
        }
        markImageUsage(material.normalTexture.index, dataUsage);
        markImageUsage(material.pbrMetallicRoughness.metallicRoughnessTexture.index, dataUsage);
    }

    std::vector<uint32_t> textureIds(model.images.size(), TEXTURE_ID_INVALID);
    std::vector<uint32_t> linearTextureIds(model.images.size(), TEXTURE_ID_INVALID);
    const auto loadImage = [&](size_t imageIdx, bool linear) {
        auto& id = (linear ? linearTextureIds : textureIds)[imageIdx];
        if (id == TEXTURE_ID_INVALID)
        {
            auto& image = model.images[imageIdx];
            const bool needsOtherUpload = (imageUsage[imageIdx] & (linear ? colorUsage : dataUsage)) &&
                (linear ? textureIds : linearTextureIds)[imageIdx] == TEXTURE_ID_INVALID;
            auto pixels = needsOtherUpload ? std::vector<uint8_t>(image.image) : std::move(image.image);
            id = scene.addTexture(std::move(pixels), image.width, image.height,
                                  linear ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        }
        return id;
    };
    const auto loadColorImage = [&](size_t imageIdx) { return loadImage(imageIdx, false); };

    // An image may be used as both color and data. Cache a distinct linear upload by image,
    // rather than changing the interpretation of the existing base-color/emission descriptor.
    const auto loadDataTexture = [&](int textureIdx, int texCoord) {
        if (textureIdx < 0)
        {
            return TEXTURE_ID_INVALID;
        }
        if (texCoord != 0)
        {
            throw std::runtime_error("Normal/roughness textures currently require TEXCOORD_0");
        }
        if (static_cast<size_t>(textureIdx) >= model.textures.size())
        {
            throw std::runtime_error("Invalid glTF data texture index");
        }
        const int imageIdx = model.textures[textureIdx].source;
        if (imageIdx < 0 || static_cast<size_t>(imageIdx) >= model.images.size())
        {
            throw std::runtime_error("Invalid glTF data texture image");
        }
        return loadImage(imageIdx, true);
    };

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
                    material.auxTextureId = loadColorImage(imgIdx);
                }
            }
        }

        // Emission is independent of the scattering lobes: an emissive material keeps whatever
        // lobes its PBR parameters describe, and a pure emitter is simply one whose base color is
        // black and whose specular is off.
        bool hasDiffuse = false;
        bool hasGlossyReflection = true;
        bool hasGlossyTransmission = false;

        const auto& pbr = gltfMat.pbrMetallicRoughness;
        // tinygltf's defaults match the glTF spec defaults, so this is correct even when the
        // pbrMetallicRoughness struct is absent
        material.roughness = static_cast<float>(pbr.roughnessFactor);
        material.normalTextureId = loadDataTexture(gltfMat.normalTexture.index, gltfMat.normalTexture.texCoord);
        material.normalScale = static_cast<float>(gltfMat.normalTexture.scale);
        material.roughnessTextureId = loadDataTexture(pbr.metallicRoughnessTexture.index,
                                                     pbr.metallicRoughnessTexture.texCoord);
        // This is a super scuffed way of determining whether the material has the pbrMetallicRoughness struct.
        // Ideally, I would use some JSON utils to check this for real. But this works for now.
        const bool hasPbr = !(pbr.metallicFactor == 1.0 && pbr.roughnessFactor == 1.0) ||
                            pbr.metallicRoughnessTexture.index >= 0;
        // Use metallicFactor to determine if material is metallic (specular-only) or dielectric (can have diffuse)
        // metallicFactor == 1.0 (default) = metallic/specular only
        // metallicFactor == 0 = dielectric, can have diffuse
        const bool isMetallic = pbr.metallicFactor >= 1.0;
        if (hasPbr)
        {
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
                            material.baseColorTextureId = loadColorImage(imgIdx);
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
                        if (isMetallic)
                        {
                            hasGlossyReflection = true;
                        }
                        else
                        {
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
            if (!hasPbr)
            {
                material.roughness = 0.f; // glTF's default roughness of 1 would make unspecified glass fully rough
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
            const Accessor* tangentAccessor = nullptr;
            if (prim.material >= 0 && static_cast<size_t>(prim.material) < model.materials.size())
            {
                const auto& mat = model.materials[prim.material];
                if ((mat.normalTexture.index >= 0 || mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) && !uvAccessor)
                {
                    throw std::runtime_error("Normal/roughness mapped glTF primitive has no TEXCOORD_0");
                }
                if (mat.normalTexture.index >= 0)
                {
                    const auto it = prim.attributes.find("TANGENT");
                    if (it == prim.attributes.end())
                    {
                        throw std::runtime_error("Normal mapped glTF requires TANGENT; export with tangents enabled");
                    }
                    tangentAccessor = &model.accessors[it->second];
                }
            }
            if (tangentAccessor && (tangentAccessor->count != vertCount || tangentAccessor->type != TINYGLTF_TYPE_VEC4 ||
                                    tangentAccessor->componentType != TINYGLTF_COMPONENT_TYPE_FLOAT))
            {
                throw std::runtime_error("glTF TANGENT must be a float VEC4 per vertex");
            }
            const auto* tangentData = tangentAccessor ? readAccessorData(*tangentAccessor) : nullptr;
            const size_t tangentStride = tangentAccessor ? getStride(*tangentAccessor) : 0;
            if (tangentAccessor)
            {
                instance->host_tangents.resize(vertCount);
            }
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

                host_verts[v] = {
                    { p[0], p[1], p[2] },
                    Util::octEncode({ n[0], n[1], n[2] }),
                    uv,
                };
                if (tangentAccessor)
                {
                    const float* t = reinterpret_cast<const float*>(tangentData + tangentStride * v);
                    instance->host_tangents[v] = { Util::octEncode({ t[0], t[1], t[2] }), t[3] };
                }
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
