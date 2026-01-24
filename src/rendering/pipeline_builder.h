/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "dxr_common.h"

#include "debug.h"
#include "renderer.h"
#include "buffer/buffer_helper.h"

#include <vector>
#include <span>

inline D3D12_SHADER_BYTECODE makeShaderBytecode(std::span<const unsigned char> bytecode)
{
    return {
        .pShaderBytecode = bytecode.data(),
        .BytecodeLength = bytecode.size_bytes(),
    };
}

struct RtPipelineInputs
{
    const std::wstring& name;

    std::span<const unsigned char> shaderBytecode;
    std::vector<D3D12_HIT_GROUP_DESC> hitGroups;
    uint32_t maxPayloadSizeBytes;
    ID3D12RootSignature* rootSig;
    ComPtr<ID3D12StateObject>& pso;

    ComPtr<ID3D12Resource>& dev_shaderIds;
    const std::wstring& rgsShaderName;
    const std::wstring& missShaderName;
    D3D12_DISPATCH_RAYS_DESC& dispatchDesc;
};

void makeRtPipeline(const RtPipelineInputs& inputs)
{
    D3D12_DXIL_LIBRARY_DESC lib = {
        .DXILLibrary = makeShaderBytecode(inputs.shaderBytecode),
    };

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {
        .MaxPayloadSizeInBytes = inputs.maxPayloadSizeBytes,
        .MaxAttributeSizeInBytes = 8,
    };

    D3D12_GLOBAL_ROOT_SIGNATURE globalSig = {
        inputs.rootSig,
    };

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg = {
        .MaxTraceRecursionDepth = 1,
    };

    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    {
        subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, .pDesc = &lib });
        subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, .pDesc = &shaderCfg });
        subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, .pDesc = &globalSig });
        subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, .pDesc = &pipelineCfg });

        for (const auto& hitGroup : inputs.hitGroups)
        {
            subobjects.push_back({ .Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, .pDesc = &hitGroup });
        }
    }

    D3D12_STATE_OBJECT_DESC desc = {
        .Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
        .NumSubobjects = static_cast<uint32_t>(subobjects.size()),
        .pSubobjects = subobjects.data(),
    };
    CHECK_HRESULT(Renderer::device->CreateStateObject(&desc, IID_PPV_ARGS(&inputs.pso)));
    const std::wstring psoName = inputs.name + L"_pso";
    inputs.pso->SetName(psoName.c_str());

    const uint32_t shaderIdsSizeBytes =
        2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT + inputs.hitGroups.size() * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    inputs.dev_shaderIds = BufferHelper::createBasicBuffer(shaderIdsSizeBytes, &UPLOAD_HEAP);
    const std::wstring shaderIdsName = inputs.name + L"_shaderIds";
    inputs.dev_shaderIds->SetName(shaderIdsName.c_str());

    ComPtr<ID3D12StateObjectProperties> props;
    inputs.pso.As(&props);

    uint8_t* host_shaderIds;
    inputs.dev_shaderIds->Map(0, nullptr, reinterpret_cast<void**>(&host_shaderIds));

    auto writeShaderId = [&](const wchar_t* name, const uint32_t incrementSizeBytes)
    {
        void* id = props->GetShaderIdentifier(name);
        ASSERT(id != nullptr);
        memcpy(host_shaderIds, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        host_shaderIds += incrementSizeBytes;
    };

    writeShaderId(inputs.rgsShaderName.c_str(), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    writeShaderId(inputs.missShaderName.c_str(), D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    for (const auto& hitGroup : inputs.hitGroups)
    {
        writeShaderId(hitGroup.HitGroupExport, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }

    inputs.dev_shaderIds->Unmap(0, nullptr);

    inputs.dispatchDesc = {
        .RayGenerationShaderRecord = {
            .StartAddress = inputs.dev_shaderIds->GetGPUVirtualAddress(),
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
        .MissShaderTable = {
            .StartAddress = inputs.dev_shaderIds->GetGPUVirtualAddress() + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
        .HitGroupTable = {
            .StartAddress = inputs.dev_shaderIds->GetGPUVirtualAddress() + 2 * D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,
            .SizeInBytes = inputs.hitGroups.size() * D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            .StrideInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        },
        .Depth = 1,
    };
}
