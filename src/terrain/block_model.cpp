// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "block_model.h"

#include "logger.h"
#include "util/packing.h"

#include <tiny_gltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace BlockModels
{
namespace
{
std::vector<Model> models;
std::unordered_map<std::string, uint32_t> ids;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Copy accessor components rather than dereferencing potentially unaligned data.
struct AccessorView
{
    const unsigned char* data;
    size_t stride;
    size_t count;
    int componentType;
};

AccessorView accessor(const tinygltf::Model& gltf, int index, int type)
{
    const auto& a = gltf.accessors.at(index);
    require(a.type == type && !a.normalized && !a.sparse.isSparse, "unsupported model accessor");
    const auto& view = gltf.bufferViews.at(a.bufferView);
    const auto& buffer = gltf.buffers.at(view.buffer);
    const int componentSize = tinygltf::GetComponentSizeInBytes(a.componentType);
    const int components = tinygltf::GetNumComponentsInType(a.type);
    require(componentSize > 0 && components > 0, "invalid accessor component type");
    const size_t elementSize = static_cast<size_t>(componentSize * components);
    const size_t stride = view.byteStride ? view.byteStride : elementSize;
    require(stride >= elementSize && a.count > 0, "empty or invalid model accessor");
    require(view.byteOffset <= buffer.data.size() && view.byteLength <= buffer.data.size() - view.byteOffset,
            "model buffer view out of bounds");
    require(a.byteOffset <= view.byteLength && elementSize <= view.byteLength - a.byteOffset,
            "model accessor out of bounds");
    require(a.count - 1 <= (view.byteLength - a.byteOffset - elementSize) / stride,
            "model accessor exceeds its buffer view");
    return { buffer.data.data() + view.byteOffset + a.byteOffset, stride, a.count, a.componentType };
}

glm::vec3 quarterTurn(glm::vec3 v, unsigned turn)
{
    switch (turn)
    {
        case 1: return { v.z, v.y, -v.x };
        case 2: return { -v.x, v.y, -v.z };
        case 3: return { -v.z, v.y, v.x };
        default: return v;
    }
}
} // namespace

Model readGlb(const std::filesystem::path& path)
{
    try
    {
        require(path.extension() == ".glb", "block model must be a GLB");
        tinygltf::TinyGLTF loader;
        tinygltf::Model gltf;
        std::string error, warning;
        if (!loader.LoadBinaryFromFile(&gltf, &error, &warning, path.string()))
            throw std::runtime_error("cannot read GLB: " + error);
        require(gltf.animations.empty() && gltf.skins.empty() && gltf.extensionsRequired.empty(),
                "block models must be static, uncompressed geometry without required extensions");
        require(!gltf.scenes.empty(), "block model has no scene");

        Model result;
        std::vector<bool> visiting(gltf.nodes.size(), false);
        std::function<void(int, const glm::mat4&)> visit;
        visit = [&](int nodeIdx, const glm::mat4& parent)
        {
            const auto& node = gltf.nodes.at(nodeIdx);
            require(!visiting.at(nodeIdx), "cycle in model node hierarchy");
            visiting[nodeIdx] = true;
            require(node.skin < 0 && node.weights.empty(), "skinned or morphed block model");
            glm::mat4 local(1.f);
            if (!node.matrix.empty())
            {
                require(node.matrix.size() == 16, "invalid model transform");
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) local[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
            }
            else
            {
                if (!node.translation.empty())
                {
                    require(node.translation.size() == 3, "invalid model translation");
                    local = glm::translate(local, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
                }
                if (!node.rotation.empty())
                {
                    require(node.rotation.size() == 4, "invalid model rotation");
                    glm::quat q(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
                    require(glm::length(q) > 0.f, "invalid zero quaternion");
                    local *= glm::mat4_cast(glm::normalize(q));
                }
                if (!node.scale.empty())
                {
                    require(node.scale.size() == 3, "invalid model scale");
                    local = glm::scale(local, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
                }
            }
            const glm::mat4 transform = parent * local;
            const float determinant = glm::determinant(glm::mat3(transform));
            require(std::isfinite(determinant) && std::abs(determinant) > 1e-12f, "singular model transform");
            const glm::mat3 normalTransform = glm::transpose(glm::inverse(glm::mat3(transform)));
            if (node.mesh >= 0)
            {
                const auto& mesh = gltf.meshes.at(node.mesh);
                for (const auto& primitive : mesh.primitives)
                {
                    require(primitive.mode == TINYGLTF_MODE_TRIANGLES && primitive.targets.empty(),
                            "block models require triangle primitives without morph targets");
                    const auto positions = accessor(gltf, primitive.attributes.at("POSITION"), TINYGLTF_TYPE_VEC3);
                    const auto normals = accessor(gltf, primitive.attributes.at("NORMAL"), TINYGLTF_TYPE_VEC3);
                    const auto uvs = accessor(gltf, primitive.attributes.at("TEXCOORD_0"), TINYGLTF_TYPE_VEC2);
                    require(positions.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
                            normals.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && uvs.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
                            positions.count == normals.count && positions.count == uvs.count,
                            "block models require matching float position, normal and UV attributes");
                    require(positions.count <= std::numeric_limits<uint32_t>::max() - result.rotations[0].size(),
                            "too many model vertices");
                    const auto base = static_cast<uint32_t>(result.rotations[0].size());
                    for (size_t i = 0; i < positions.count; ++i)
                    {
                        float p[3], n[3], uv[2];
                        std::memcpy(p, positions.data + i * positions.stride, sizeof(p));
                        std::memcpy(n, normals.data + i * normals.stride, sizeof(n));
                        std::memcpy(uv, uvs.data + i * uvs.stride, sizeof(uv));
                        const glm::vec3 position = glm::vec3(transform * glm::vec4(p[0], p[1], p[2], 1.f));
                        glm::vec3 normal = normalTransform * glm::vec3(n[0], n[1], n[2]);
                        require(std::isfinite(glm::length(normal)) && glm::length(normal) > 0.f, "invalid model normal");
                        normal = glm::normalize(normal);
                        for (int c = 0; c < 3; ++c) require(std::isfinite(position[c]), "non-finite model position");
                        require(position.x >= -.5001f && position.x <= .5001f && position.z >= -.5001f &&
                                position.z <= .5001f && position.y >= -.0001f && position.y <= 1.0001f,
                                "decorator model must fit one block, centered at its base (Y up)");
                        require(std::isfinite(uv[0]) && std::isfinite(uv[1]) && uv[0] >= 0.f && uv[0] <= 1.f &&
                                uv[1] >= 0.f && uv[1] <= 1.f, "model UVs must stay within the block texture");
                        for (unsigned turn = 0; turn < 4; ++turn)
                        {
                            const auto rp = quarterTurn(position, turn);
                            const auto rn = quarterTurn(normal, turn);
                            result.rotations[turn].push_back({ { rp.x, rp.y, rp.z },
                                Util::octEncode({ rn.x, rn.y, rn.z }), Util::packFloat2ToUint(uv[0], uv[1]) });
                        }
                    }
                    const size_t firstIndex = result.indices.size();
                    if (primitive.indices >= 0)
                    {
                        const auto indices = accessor(gltf, primitive.indices, TINYGLTF_TYPE_SCALAR);
                        require(indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
                                indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ||
                                indices.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, "invalid index type");
                        const size_t size = tinygltf::GetComponentSizeInBytes(indices.componentType);
                        for (size_t i = 0; i < indices.count; ++i)
                        {
                            uint32_t index = 0;
                            std::memcpy(&index, indices.data + i * indices.stride, size);
                            require(index < positions.count, "model index out of bounds");
                            result.indices.push_back(base + index);
                        }
                    }
                    else
                        for (uint32_t i = 0; i < positions.count; ++i) result.indices.push_back(base + i);
                    require((result.indices.size() - firstIndex) % 3 == 0, "incomplete triangle primitive");
                    for (size_t i = firstIndex; i < result.indices.size(); i += 3)
                    {
                        if (determinant < 0) std::swap(result.indices[i + 1], result.indices[i + 2]);
                        const auto point = [&](size_t j) {
                            const auto& p = result.rotations[0][result.indices[j]].pos_OS;
                            return glm::vec3(p.x, p.y, p.z);
                        };
                        require(glm::length(glm::cross(point(i + 1) - point(i), point(i + 2) - point(i))) > 1e-10f,
                                "degenerate model triangle");
                    }
                }
            }
            for (int child : node.children) visit(child, transform);
            visiting[nodeIdx] = false;
        };
        const auto& scene = gltf.scenes.at(gltf.defaultScene >= 0 ? gltf.defaultScene : 0);
        for (int root : scene.nodes) visit(root, glm::mat4(1.f));
        require(!result.indices.empty(), "block model has no triangles");
        return result;
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("block model " + path.generic_string() + ": " + e.what());
    }
}

void clear() { models.clear(); ids.clear(); }

uint32_t load(const std::filesystem::path& path)
{
    const auto key = path.lexically_normal().generic_string();
    if (const auto it = ids.find(key); it != ids.end()) return it->second;
    auto model = readGlb(path);
    const auto id = static_cast<uint32_t>(models.size());
    Logger::log("Loaded block model %s: %zu vertices, %zu triangles, 4 cached rotations",
                key.c_str(), model.rotations[0].size(), model.indices.size() / 3);
    models.push_back(std::move(model));
    ids.emplace(key, id);
    return id;
}

const Model& get(uint32_t id) { return models.at(id); }
} // namespace BlockModels
