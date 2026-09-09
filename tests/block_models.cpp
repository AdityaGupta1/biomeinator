// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aditya Gupta
#include "terrain/block.h"
#include "terrain/block_model.h"
#include <tiny_gltf.h>
#include <stb_image.h>
#include <glm/glm.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>

static void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static glm::vec3 normal(uint32_t packed)
{
    const float x = std::max(-1.f, static_cast<int16_t>(packed & 0xffff) / 32767.f);
    const float y = std::max(-1.f, static_cast<int16_t>(packed >> 16) / 32767.f);
    glm::vec3 n(x, y, 1.f - std::abs(x) - std::abs(y));
    if (n.z < 0) { n.x = (1.f - std::abs(y)) * (x >= 0 ? 1 : -1); n.y = (1.f - std::abs(x)) * (y >= 0 ? 1 : -1); }
    return glm::normalize(n);
}

int main()
{
    try
    {
        namespace fs = std::filesystem;
        const fs::path assets = fs::path(CMAKE_SOURCE_DIR) / "assets/blocks";
        Blocks::init();
        for (const auto shape : { BlockShape::X_SHAPED, BlockShape::DECORATOR_CUSTOM })
        for (const auto neighborType : { BlockType::SOLID, BlockType::TRANSPARENT_CUTOUT })
        for (int face = 0; face < 6; ++face)
        {
            check(blockFaceVisible(BlockType::SOLID, BlockShape::CUBE, neighborType, shape, face), "solid face against decorator");
            check(blockFaceVisible(BlockType::TRANSPARENT_CUTOUT, BlockShape::CUBE, neighborType, shape, face), "cutout face against decorator");
            check(blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, neighborType, shape, face), "glass face against decorator");
            check(!blockFaceVisible(BlockType::WATER, BlockShape::CUBE, neighborType, shape, face), "water stays hidden against decorator");
        }
        for (int face = 0; face < 6; ++face)
        {
            check(!blockFaceVisible(BlockType::SOLID, BlockShape::CUBE, BlockType::SOLID, BlockShape::CUBE, face), "solid shared boundary");
            check(!blockFaceVisible(BlockType::TRANSPARENT_CUTOUT, BlockShape::CUBE, BlockType::SOLID, BlockShape::CUBE, face), "cutout against solid");
            check(blockFaceVisible(BlockType::TRANSPARENT_CUTOUT, BlockShape::CUBE, BlockType::TRANSPARENT_CUTOUT, BlockShape::CUBE, face) == (face == 0 || face == 1 || face == 4), "one owner per cutout boundary");
            check(blockFaceVisible(BlockType::WATER, BlockShape::LIQUID_TOP, BlockType::SOLID, BlockShape::CUBE, face) == (face == 4), "water top exception");
            check(blockFaceVisible(BlockType::SOLID, BlockShape::CUBE, BlockType::SOLID, BlockShape::LIQUID_TOP, face) == (face != 4), "full cube against lowered solid");
            check(blockFaceVisible(BlockType::SOLID, BlockShape::LIQUID_TOP, BlockType::SOLID, BlockShape::CUBE, face) == (face == 4), "lowered solid against full cube");
            for (const auto type : { BlockType::SOLID, BlockType::TRANSPARENT_CUTOUT, BlockType::WATER })
                check(blockFaceVisible(type, BlockShape::CUBE, BlockType::AIR, BlockShape::CUBE, face), "faces against air");
        }
        for (int face = 0; face < 6; ++face)
        {
            check(!blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::GLASS, BlockShape::CUBE, face), "glass internal boundary");
            check(!blockFaceVisible(BlockType::GLASS, BlockShape::CUBE, BlockType::SOLID, BlockShape::CUBE, face), "buried glass boundary");
            check(blockFaceVisible(BlockType::SOLID, BlockShape::CUBE, BlockType::GLASS, BlockShape::CUBE, face), "solid remains visible through glass");
        }
        check(Blocks::getBlockData(Block::CRYSTAL_CORE).proceduralColor, "crystal procedural color survives parsing");
        const auto& ore = Blocks::getBlockData(Block::CRACKED_BASALT_CRYSTAL_ORE);
        check(ore.type == BlockType::SOLID && ore.emitsLight && ore.proceduralColor, "crystal ore shading and light registration");
        {
            int w, h, c;
            auto* mask = stbi_load((assets / "textures/cracked_basalt_crystal_ore.aux.png").string().c_str(), &w, &h, &c, 4);
            check(mask && w == 16 && h == 16, "crystal ore mask size");
            int emissive = 0;
            for (int i = 0; i < 256; ++i) { emissive += mask[i*4] > 0; check(mask[i*4+3] == 255, "opaque ore mask"); }
            check(emissive > 0 && emissive < 256, "ore mask separates emission from basalt");
            stbi_image_free(mask);
        }
        const auto& shard = Blocks::getBlockData(Block::CRYSTAL_SHARD);
        check(shard.shape == BlockShape::DECORATOR_CUSTOM && shard.proceduralColor, "crystal shard model uses procedural ramp");
        check(Blocks::getTextureNames().at(shard.texSlices[0]) == "crystal_core", "crystal shard uses core atlas");
        const auto& shardMesh = BlockModels::get(shard.modelIdx);
        check(shardMesh.indices.size() == 36 * 3, "three rectangular crystal prisms");
        float shardMinY = 1.f;
        for (const auto& v : shardMesh.rotations[0]) shardMinY = std::min(shardMinY, v.pos_OS.y);
        check(shardMinY < 0.f && shardMinY >= -.125f, "crystal bases are buried within allowance");
        const fs::path definitions = fs::path(CMAKE_BINARY_DIR)/"test_output/block_models";
        fs::create_directories(definitions);
        const auto definition = definitions/"invalid_block.json";
        for (const char* json : { R"({"shape":"decorator_custom","textures":{}})",
                                  R"({"shape":"decorator_custom","type":"invalid"})" })
        {
            { std::ofstream out(definition); out << json; }
            bool rejected = false;
            try { Blocks::readBlockJson(definition); } catch (const std::exception&) { rejected = true; }
            check(rejected, "custom definition errors before model loading must throw");
        }
        { std::ofstream out(definition); out << R"({"type":"water","translucent":"invalid"})"; }
        const auto fallback = Blocks::readBlockJson(definition);
        check(fallback.type == BlockType::SOLID && !fallback.translucent, "failed definitions must not publish partial metadata");

        for (const auto block : { Block::BROWN_MUSHROOM, Block::GLOWSHROOM_YELLOW })
        {
            const bool glow = block == Block::GLOWSHROOM_YELLOW;
            const auto& data = Blocks::getBlockData(block);
            check(data.shape == BlockShape::DECORATOR_CUSTOM && data.type == BlockType::SOLID, "model block classification");
            check(!data.translucent && !data.emitsLight, "mushrooms must not transmit or register sampled lights");
            check(data.numRotationsY == (glow ? 4 : 1), "rotation configuration");
            const auto& mesh = BlockModels::get(data.modelIdx);
            check(mesh.indices.size() == (glow ? 108 : 36) * 3, "exported triangle count");
            float minY = 1;
            for (size_t i = 0; i < mesh.rotations[0].size(); ++i)
            {
                const auto& v = mesh.rotations[0][i];
                minY = std::min(minY, v.pos_OS.y);
                glm::vec3 p(v.pos_OS.x, v.pos_OS.y, v.pos_OS.z), n = normal(v.packedNor);
                for (unsigned turn = 0; turn < 4; ++turn)
                {
                    const auto& r = mesh.rotations[turn][i];
                    check(glm::length(p - glm::vec3(r.pos_OS.x, r.pos_OS.y, r.pos_OS.z)) < 1e-6f, "quarter-turn position");
                    check(glm::dot(n, normal(r.packedNor)) > .9999f, "quarter-turn normal");
                    check(r.packedUv == v.packedUv, "rotation must preserve UVs");
                    p = { p.z, p.y, -p.x }; n = { n.z, n.y, -n.x };
                }
            }
            check(std::abs(minY) < 1e-6f, "model must touch ground");
            for (size_t i = 0; i < mesh.indices.size(); i += 3)
            {
                const auto& a = mesh.rotations[0].at(mesh.indices[i]);
                const auto& b = mesh.rotations[0].at(mesh.indices[i+1]);
                const auto& c = mesh.rotations[0].at(mesh.indices[i+2]);
                const glm::vec3 ab(b.pos_OS.x-a.pos_OS.x,b.pos_OS.y-a.pos_OS.y,b.pos_OS.z-a.pos_OS.z);
                const glm::vec3 ac(c.pos_OS.x-a.pos_OS.x,c.pos_OS.y-a.pos_OS.y,c.pos_OS.z-a.pos_OS.z);
                check(glm::dot(glm::normalize(glm::cross(ab,ac)),normal(a.packedNor)) > .999f, "triangle winding and hard normals");
            }
            const auto textureName = Blocks::getTextureNames().at(data.texSlices[0]);
            int w,h,channels;
            auto* pixels = stbi_load((assets / "textures" / (textureName + ".png")).string().c_str(), &w,&h,&channels,4);
            check(pixels && w==16 && h==16, "16px atlas");
            for (int i=0;i<256;++i) check(pixels[i*4+3]==255, "opaque custom model texture");
            stbi_image_free(pixels);
            if (glow)
            {
                pixels = stbi_load((assets / "textures" / (textureName + ".aux.png")).string().c_str(), &w,&h,&channels,4);
                check(pixels && w==16 && h==16, "glow atlas mask");
                for(int y=0;y<16;++y) for(int x=0;x<16;++x)
                    check(pixels[(y*16+x)*4]==(x<8 ? 255:0), "caps emissive, stems not emissive");
                stbi_image_free(pixels);
            }
        }
        const auto first = BlockModels::load(assets / "models/brown_mushroom.glb");
        check(first == BlockModels::load(assets / "models/brown_mushroom.glb"), "load once cache");

        tinygltf::TinyGLTF io;
        tinygltf::Model fixture;
        std::string error, warning;
        check(io.LoadBinaryFromFile(&fixture,&error,&warning,(assets/"models/brown_mushroom.glb").string()), "fixture load");
        const fs::path temp = fs::path(CMAKE_BINARY_DIR)/"test_output/block_models";
        fs::create_directories(temp);
        const auto reject = [&](tinygltf::Model invalid, const char* name)
        {
            const auto file = temp / (std::string(name)+".glb");
            check(io.WriteGltfSceneToFile(&invalid,file.string(),true,true,false,true), "fixture write");
            bool rejected=false;
            try { BlockModels::readGlb(file); } catch(const std::exception&) { rejected=true; }
            check(rejected,name);
        };
        auto invalid = fixture;
        invalid.meshes[0].primitives[0].attributes.erase("NORMAL");
        reject(invalid,"missing_normals");
        invalid = fixture;
        invalid.meshes[0].primitives[0].mode = TINYGLTF_MODE_LINE;
        reject(invalid,"non_triangles");
        invalid = fixture;
        invalid.accessors[invalid.meshes[0].primitives[0].attributes.at("POSITION")].count = 999999;
        reject(invalid,"accessor_bounds");
        invalid = fixture;
        const int root = invalid.scenes[0].nodes[0];
        invalid.nodes[root].children.push_back(root);
        reject(invalid,"node_cycle");
        invalid = fixture;
        invalid.nodes[root].translation = { 10, 0, 0 };
        reject(invalid,"outside_block");

        // Parent transforms are flattened; negative scale must also reverse winding.
        auto transformed = fixture;
        tinygltf::Node parent;
        parent.children = transformed.scenes[0].nodes;
        parent.scale = { -.5, .5, .5 };
        transformed.scenes[0].nodes = { static_cast<int>(transformed.nodes.size()) };
        transformed.nodes.push_back(parent);
        const auto file = temp/"parent_transform.glb";
        check(io.WriteGltfSceneToFile(&transformed,file.string(),true,true,false,true), "transform fixture write");
        const auto result = BlockModels::readGlb(file);
        const auto original = BlockModels::readGlb(assets/"models/brown_mushroom.glb");
        check(std::abs(result.rotations[0][0].pos_OS.x + .5f*original.rotations[0][0].pos_OS.x)<1e-6f, "parent transform flattened");
        check(result.indices[1]==original.indices[2] && result.indices[2]==original.indices[1], "mirror reverses winding");
        std::cout << "Block model tests passed (assets, rotations, normals, opacity, emission, culling, cache, malformed GLBs, hierarchy).\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
