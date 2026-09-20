// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "water_displacer.h"

#include "common/common_registers.h"
#include "common/common_settings.h"
#include "buffer/committed_managed_buffer.h"
#include "buffer/to_free_list.h"
#include "renderer/pipeline_builder.h"
#include "renderer/renderer_internal.h"
#include "renderer/shaders.h"
#include "util/util.h"

#include <array>
#include <cmath>

namespace WaterDisplacer
{

using Renderer::makeParam; // for MAKE_PARAM

namespace
{

enum class WaterDisplaceParam
{
    CONSTANTS,

    INSTANCES,

    VERTS_OUT,

    COUNT
};

#define WATER_DISPLACE_PARAM_IDX(name) static_cast<uint32_t>(WaterDisplaceParam::name)

struct WaterDisplaceConstants
{
    uint32_t numInstances;
    uint32_t numVerts;
    float waveTime;
    uint32_t pad0;
    WaveFadeParams waveFade;
};

ComPtr<ID3D12RootSignature> rootSig{ nullptr };
ComPtr<ID3D12PipelineState> pso{ nullptr };

CommittedManagedBuffer instancesUploadBuffer{
    &UPLOAD_HEAP,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    {
        .isResizable = true,
        .isMapped = true,
    },
};

// CPU mirror of waveHeight() in shaders/common/water_waves.hlsli (displacement only, no
// shading-normal noise); constants are shared via common_settings.h, but the math must
// match the shader exactly.
inline constexpr float swellStrengths[WATER_SWELL_WAVE_COUNT] = WATER_SWELL_STRENGTHS;
inline constexpr glm::vec2 swellFreqs[WATER_SWELL_WAVE_COUNT] = WATER_SWELL_FREQS;
inline constexpr float swellSpeeds[WATER_SWELL_WAVE_COUNT] = WATER_SWELL_SPEEDS;

inline constexpr float chopStrengths[WATER_CHOP_WAVE_COUNT] = WATER_CHOP_STRENGTHS;
inline constexpr glm::vec2 chopFreqs[WATER_CHOP_WAVE_COUNT] = WATER_CHOP_FREQS;
inline constexpr float chopSpeeds[WATER_CHOP_WAVE_COUNT] = WATER_CHOP_SPEEDS;

inline constexpr glm::vec2 sineChopFreqs[2] = WATER_SINE_CHOP_FREQS;
inline constexpr glm::vec2 sineChopSpeeds = WATER_SINE_CHOP_SPEEDS;

float waveHeight(const glm::vec2 posXZ_WS, const float waveTime)
{
    float height = 0.f;
    for (int i = 0; i < WATER_SWELL_WAVE_COUNT; ++i)
    {
        height += swellStrengths[i] * std::sin(glm::dot(posXZ_WS, swellFreqs[i]) + swellSpeeds[i] * waveTime);
    }

    const float phaseA = glm::dot(posXZ_WS, sineChopFreqs[0]) + sineChopSpeeds.x * waveTime;
    const float phaseB = glm::dot(posXZ_WS, sineChopFreqs[1]) + sineChopSpeeds.y * waveTime;
    const float envelope = 0.5f + 0.5f * std::sin(phaseA) * std::sin(phaseB);

    float chop = 0.f;
    for (int j = 0; j < WATER_CHOP_WAVE_COUNT; ++j)
    {
        chop += chopStrengths[j] * std::sin(glm::dot(posXZ_WS, chopFreqs[j]) + chopSpeeds[j] * waveTime);
    }

    return height + envelope * chop;
}

} // namespace

void init()
{
    std::array<D3D12_ROOT_PARAMETER1, WATER_DISPLACE_PARAM_IDX(COUNT)> params;
    params[WATER_DISPLACE_PARAM_IDX(CONSTANTS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {
            .ShaderRegister = WATER_DISPLACE_REGISTER_CONSTANTS,
            .RegisterSpace = WATER_DISPLACE_REGISTER_SPACE,
            .Num32BitValues = sizeof(WaterDisplaceConstants) / 4,
        },
    };
    params[WATER_DISPLACE_PARAM_IDX(INSTANCES)] = MAKE_PARAM(SRV, WATER_DISPLACE, INSTANCES);
    params[WATER_DISPLACE_PARAM_IDX(VERTS_OUT)] = MAKE_PARAM(UAV, WATER_DISPLACE, VERTS_OUT);

    Renderer::serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                              nullptr, 0, rootSig);
    rootSig->SetName(L"waterDisplaceRootSig");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = makeShaderBytecode(getShader("water_displace_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    pso->SetName(L"waterDisplacePso");

    instancesUploadBuffer.setName(L"waterDisplaceInstancesUploadBuffer");
    instancesUploadBuffer.init(1 << 16 /*bytes*/);
}

void dispatch(ID3D12GraphicsCommandList4* cmdList,
              ToFreeList& toFreeList,
              D3D12_GPU_VIRTUAL_ADDRESS dev_vertsAddress,
              float waveTime,
              const WaveFadeParams& waveFade,
              const std::vector<DispatchInputs>& allInputs)
{
    if (allInputs.empty())
    {
        return;
    }

    std::vector<WaterDisplaceInstance> instances;
    instances.reserve(allInputs.size());
    uint32_t numVerts = 0;
    for (const DispatchInputs& inputs : allInputs)
    {
        instances.push_back({
            .firstVert = numVerts,
            .vertsBufferOffset = inputs.vertsBufferOffset,
            .vertCount = inputs.vertCount,
            .waveScale = inputs.waveScale,
            .transformOffset = { inputs.transformOffset.x, inputs.transformOffset.y, inputs.transformOffset.z },
        });
        numVerts += inputs.vertCount;
    }

    const ManagedBufferSection instancesSection = instancesUploadBuffer.copyFromHostVector(cmdList, toFreeList, instances);
    toFreeList.pushManagedBufferSection(instancesSection);

    cmdList->SetPipelineState(pso.Get());
    cmdList->SetComputeRootSignature(rootSig.Get());

    const WaterDisplaceConstants constants = {
        .numInstances = static_cast<uint32_t>(instances.size()),
        .numVerts = numVerts,
        .waveTime = waveTime,
        .waveFade = waveFade,
    };
    cmdList->SetComputeRoot32BitConstants(WATER_DISPLACE_PARAM_IDX(CONSTANTS),
                                          sizeof(WaterDisplaceConstants) / 4, &constants, 0);
    cmdList->SetComputeRootShaderResourceView(WATER_DISPLACE_PARAM_IDX(INSTANCES), instancesSection.getGpuVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(WATER_DISPLACE_PARAM_IDX(VERTS_OUT), dev_vertsAddress);

    cmdList->Dispatch(Util::calculateDispatchSize(numVerts, WATER_DISPLACE_WORKGROUP_SIZE), 1, 1);
}

void destroy()
{
    instancesUploadBuffer.reset();
    pso.Reset();
    rootSig.Reset();
}

// Sampled at the camera, where the distance fade is 1, so it has no fade term
float sampleMeshWaveOffsetY(const glm::ivec2 blockXZ_WS, const glm::vec2 blockFraction, const float waveTime)
{
    const float h00 = waveHeight(glm::vec2(blockXZ_WS), waveTime);
    const float h10 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(1, 0)), waveTime);
    const float h01 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(0, 1)), waveTime);
    const float h11 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(1, 1)), waveTime);

    const float fx = blockFraction.x;
    const float fz = blockFraction.y;
    return (fx >= fz)
        ? h00 + (h10 - h00) * fx + (h11 - h10) * fz
        : h00 + (h11 - h01) * fx + (h01 - h00) * fz;
}

} // namespace WaterDisplacer
