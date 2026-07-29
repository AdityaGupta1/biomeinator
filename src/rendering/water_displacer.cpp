// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "water_displacer.h"

#include "common/common_registers.h"
#include "common/common_settings.h"
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

    VERTS_OUT,

    COUNT
};

#define WATER_DISPLACE_PARAM_IDX(name) static_cast<uint32_t>(WaterDisplaceParam::name)

struct WaterDisplaceConstants
{
    uint32_t vertsBufferOffset;
    uint32_t vertCount;
    int32_t transformOffsetX;
    int32_t transformOffsetZ;
    float animTime;
};

ComPtr<ID3D12RootSignature> rootSig{ nullptr };
ComPtr<ID3D12PipelineState> pso{ nullptr };

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

float waveHeight(const glm::vec2 posXZ_WS, const float time)
{
    float height = 0.f;
    for (int i = 0; i < WATER_SWELL_WAVE_COUNT; ++i)
    {
        height += swellStrengths[i] * std::sin(glm::dot(posXZ_WS, swellFreqs[i]) + swellSpeeds[i] * time);
    }

    const float phaseA = glm::dot(posXZ_WS, sineChopFreqs[0]) + sineChopSpeeds.x * time;
    const float phaseB = glm::dot(posXZ_WS, sineChopFreqs[1]) + sineChopSpeeds.y * time;
    const float envelope = 0.5f + 0.5f * std::sin(phaseA) * std::sin(phaseB);

    float chop = 0.f;
    for (int j = 0; j < WATER_CHOP_WAVE_COUNT; ++j)
    {
        chop += chopStrengths[j] * std::sin(glm::dot(posXZ_WS, chopFreqs[j]) + chopSpeeds[j] * time);
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
    params[WATER_DISPLACE_PARAM_IDX(VERTS_OUT)] = MAKE_PARAM(UAV, WATER_DISPLACE, VERTS_OUT);

    Renderer::serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                              nullptr, 0, rootSig);
    rootSig->SetName(L"waterDisplaceRootSig");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.CS = makeShaderBytecode(getShader("water_displace_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
    pso->SetName(L"waterDisplacePso");
}

void dispatch(ID3D12GraphicsCommandList4* cmdList,
              D3D12_GPU_VIRTUAL_ADDRESS dev_vertsAddress,
              float animTime,
              const std::vector<DispatchInputs>& allInputs)
{
    cmdList->SetPipelineState(pso.Get());
    cmdList->SetComputeRootSignature(rootSig.Get());

    cmdList->SetComputeRootUnorderedAccessView(WATER_DISPLACE_PARAM_IDX(VERTS_OUT), dev_vertsAddress);

    // TODO: batch into a single dispatch with an instance table if per-instance dispatch
    // overhead shows up in profiling
    for (const DispatchInputs& inputs : allInputs)
    {
        const WaterDisplaceConstants constants = {
            .vertsBufferOffset = inputs.vertsBufferOffset,
            .vertCount = inputs.vertCount,
            .transformOffsetX = inputs.transformOffsetX,
            .transformOffsetZ = inputs.transformOffsetZ,
            .animTime = animTime,
        };
        cmdList->SetComputeRoot32BitConstants(WATER_DISPLACE_PARAM_IDX(CONSTANTS),
                                              sizeof(WaterDisplaceConstants) / 4, &constants, 0);

        const uint32_t dispatchSize = Util::calculateDispatchSize(inputs.vertCount, WATER_DISPLACE_WORKGROUP_SIZE);
        cmdList->Dispatch(dispatchSize, 1, 1);
    }
}

void destroy()
{
    pso.Reset();
    rootSig.Reset();
}

float sampleMeshWaveOffsetY(const glm::ivec2 blockXZ_WS, const glm::vec2 blockFraction, const float time)
{
    const float h00 = waveHeight(glm::vec2(blockXZ_WS), time);
    const float h10 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(1, 0)), time);
    const float h01 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(0, 1)), time);
    const float h11 = waveHeight(glm::vec2(blockXZ_WS + glm::ivec2(1, 1)), time);

    const float fx = blockFraction.x;
    const float fz = blockFraction.y;
    return (fx >= fz)
        ? h00 + (h10 - h00) * fx + (h11 - h10) * fz
        : h00 + (h11 - h01) * fx + (h01 - h00) * fz;
}

} // namespace WaterDisplacer
