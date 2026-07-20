// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "sky_atmosphere.h"

#include "common/common_registers.h"
#include "common/common_settings.h"
#include "renderer/pipeline_builder.h"
#include "renderer/renderer_internal.h"
#include "renderer/rt_target.h"
#include "renderer/shaders.h"
#include "rendering/buffer/buffer_helper.h"
#include "util/util.h"

#include <array>

namespace SkyAtmosphere
{

namespace
{

enum class SkyParam
{
    CONSTANTS,

    COUNT
};

#define SKY_PARAM_IDX(name) static_cast<uint32_t>(SkyParam::name)

struct SkyConstants
{
    uint32_t lutUavIdx;
    uint32_t transmittanceLutSrvIdx;
    uint32_t multiScatteringLutSrvIdx;
    float animTime;
    float cameraY;
};

ComPtr<ID3D12RootSignature> rootSig{ nullptr };
ComPtr<ID3D12PipelineState> transmittancePso{ nullptr };
ComPtr<ID3D12PipelineState> multiScatteringPso{ nullptr };
ComPtr<ID3D12PipelineState> skyViewPso{ nullptr };

RtTarget transmittanceLut{ L"skyTransmittanceLut", DXGI_FORMAT_R16G16B16A16_FLOAT };
RtTarget multiScatteringLut{ L"skyMultiScatteringLut", DXGI_FORMAT_R16G16B16A16_FLOAT };
RtTarget skyViewLut{ L"skyViewLut", DXGI_FORMAT_R16G16B16A16_FLOAT };

bool staticLutsGenerated{ false };

} // namespace

void init()
{
    std::array<D3D12_ROOT_PARAMETER1, SKY_PARAM_IDX(COUNT)> params;
    params[SKY_PARAM_IDX(CONSTANTS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
        .Constants = {
            .ShaderRegister = SKY_REGISTER_CONSTANTS,
            .RegisterSpace = SKY_REGISTER_SPACE,
            .Num32BitValues = sizeof(SkyConstants) / 4,
        },
    };

    const D3D12_STATIC_SAMPLER_DESC lutSamplerDesc = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ShaderRegister = SKY_REGISTER_LUT_SAMPLER,
        .RegisterSpace = SKY_REGISTER_SPACE,
    };

    Renderer::serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                              &lutSamplerDesc, 1, rootSig);
    rootSig->SetName(L"skyAtmosphereRootSig");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig.Get();

    psoDesc.CS = makeShaderBytecode(getShader("transmittance_lut_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&transmittancePso)));
    transmittancePso->SetName(L"skyTransmittanceLutPso");

    psoDesc.CS = makeShaderBytecode(getShader("multi_scattering_lut_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&multiScatteringPso)));
    multiScatteringPso->SetName(L"skyMultiScatteringLutPso");

    psoDesc.CS = makeShaderBytecode(getShader("sky_view_lut_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&skyViewPso)));
    skyViewPso->SetName(L"skyViewLutPso");

    transmittanceLut.setDimensions(SKY_TRANSMITTANCE_LUT_WIDTH, SKY_TRANSMITTANCE_LUT_HEIGHT);
    transmittanceLut.init();

    multiScatteringLut.setDimensions(SKY_MULTI_SCATTERING_LUT_SIZE, SKY_MULTI_SCATTERING_LUT_SIZE);
    multiScatteringLut.init();

    skyViewLut.setDimensions(SKY_VIEW_LUT_WIDTH, SKY_VIEW_LUT_HEIGHT);
    skyViewLut.init();
}

void dispatch(ID3D12GraphicsCommandList4* cmdList, const float animTime, const float cameraY)
{
    cmdList->SetComputeRootSignature(rootSig.Get());

    if (!staticLutsGenerated)
    {
        transmittanceLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmdList->SetPipelineState(transmittancePso.Get());
        const SkyConstants transmittanceConstants = {
            .lutUavIdx = transmittanceLut.getUavIdx(),
        };
        cmdList->SetComputeRoot32BitConstants(SKY_PARAM_IDX(CONSTANTS), sizeof(SkyConstants) / 4, &transmittanceConstants, 0);
        cmdList->Dispatch(Util::calculateDispatchSize(SKY_TRANSMITTANCE_LUT_WIDTH, SKY_WORKGROUP_SIZE_X),
                          Util::calculateDispatchSize(SKY_TRANSMITTANCE_LUT_HEIGHT, SKY_WORKGROUP_SIZE_Y),
                          1);

        BufferHelper::uavBarrier(cmdList, transmittanceLut.getTarget());
        transmittanceLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        multiScatteringLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmdList->SetPipelineState(multiScatteringPso.Get());
        const SkyConstants multiScatteringConstants = {
            .lutUavIdx = multiScatteringLut.getUavIdx(),
            .transmittanceLutSrvIdx = transmittanceLut.getSrvIdx(),
        };
        cmdList->SetComputeRoot32BitConstants(SKY_PARAM_IDX(CONSTANTS), sizeof(SkyConstants) / 4, &multiScatteringConstants, 0);
        cmdList->Dispatch(Util::calculateDispatchSize(SKY_MULTI_SCATTERING_LUT_SIZE, SKY_WORKGROUP_SIZE_X),
                          Util::calculateDispatchSize(SKY_MULTI_SCATTERING_LUT_SIZE, SKY_WORKGROUP_SIZE_Y),
                          1);

        BufferHelper::uavBarrier(cmdList, multiScatteringLut.getTarget());
        staticLutsGenerated = true;
    }

    multiScatteringLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    skyViewLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetPipelineState(skyViewPso.Get());
    const SkyConstants constants = {
        .lutUavIdx = skyViewLut.getUavIdx(),
        .transmittanceLutSrvIdx = transmittanceLut.getSrvIdx(),
        .multiScatteringLutSrvIdx = multiScatteringLut.getSrvIdx(),
        .animTime = animTime,
        .cameraY = cameraY,
    };
    cmdList->SetComputeRoot32BitConstants(SKY_PARAM_IDX(CONSTANTS), sizeof(SkyConstants) / 4, &constants, 0);
    cmdList->Dispatch(Util::calculateDispatchSize(SKY_VIEW_LUT_WIDTH, SKY_WORKGROUP_SIZE_X),
                      Util::calculateDispatchSize(SKY_VIEW_LUT_HEIGHT, SKY_WORKGROUP_SIZE_Y),
                      1);

    BufferHelper::uavBarrier(cmdList, skyViewLut.getTarget());
    skyViewLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

uint32_t getTransmittanceLutSrvIdx()
{
    return transmittanceLut.getSrvIdx();
}

uint32_t getSkyViewLutSrvIdx()
{
    return skyViewLut.getSrvIdx();
}

void destroy()
{
    transmittanceLut.reset();
    multiScatteringLut.reset();
    skyViewLut.reset();
    skyViewPso.Reset();
    multiScatteringPso.Reset();
    transmittancePso.Reset();
    rootSig.Reset();
    staticLutsGenerated = false;
}

} // namespace SkyAtmosphere
