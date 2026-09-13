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
#include <cstring>

namespace SkyAtmosphere
{

namespace
{

enum class SkyParam
{
    CONSTANTS,
    GLOBAL_PARAMS,

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
    uint32_t cloudNoiseIdx;
    uint32_t cloudShapeIdx;
    float cloudCoverage;
    float cloudDensity;
    uint32_t sunSampleFrame;
    float padding[2];
    CloudSettings cloud;
};

ComPtr<ID3D12RootSignature> rootSig{ nullptr };
ComPtr<ID3D12PipelineState> transmittancePso{ nullptr };
ComPtr<ID3D12PipelineState> multiScatteringPso{ nullptr };
ComPtr<ID3D12PipelineState> skyViewPso{ nullptr };
ComPtr<ID3D12PipelineState> cloudNoisePso, cloudShapePso, cloudLightPso, cloudViewPso;
RtTarget cloudNoise{ L"cloudNoise", DXGI_FORMAT_R16G16B16A16_FLOAT };
RtTarget cloudShape{ L"cloudShape", DXGI_FORMAT_R16_FLOAT };
RtTarget cloudLight{ L"cloudOpticalDepth", DXGI_FORMAT_R16_FLOAT };
RtTarget cloudView{ L"cloudView", DXGI_FORMAT_R16G16B16A16_FLOAT };
uint32_t cloudViewWidth = 1, cloudViewHeight = 1;
CloudSettings previousCloudSettings{};
bool cloudNoiseReady = false;
float previousCoverage = -1.f, previousDensity = -1.f, previousCloudTime = -1.e20f;

RtTarget transmittanceLut{ L"skyTransmittanceLut", DXGI_FORMAT_R16G16B16A16_FLOAT };
RtTarget multiScatteringLut{ L"skyMultiScatteringLut", DXGI_FORMAT_R16G16B16A16_FLOAT };
RtTarget skyViewLut{ L"skyViewLut", DXGI_FORMAT_R16G16B16A16_FLOAT };

bool staticLutsGenerated{ false };
uint32_t sunSampleFrame = 0;

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

    params[SKY_PARAM_IDX(GLOBAL_PARAMS)] = {
        .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
        .Descriptor = { .ShaderRegister = COMMON_REGISTER_GLOBAL_PARAMS, .RegisterSpace = COMMON_REGISTER_SPACE },
    };
    static_assert(sizeof(SkyConstants) / 4 + 2 <= 64);

    const D3D12_STATIC_SAMPLER_DESC lutSamplerDesc = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ShaderRegister = SKY_REGISTER_LUT_SAMPLER,
        .RegisterSpace = SKY_REGISTER_SPACE,
    };

    std::array<D3D12_STATIC_SAMPLER_DESC, 6> samplers;
    samplers.fill(lutSamplerDesc);
    samplers[1].ShaderRegister = SKY_REGISTER_NOISE_SAMPLER;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].RegisterSpace = RT_REGISTER_SPACE;
    samplers[2].ShaderRegister = RT_REGISTER_LUT_SAMPLER;
    samplers[3] = samplers[2];
    samplers[3].ShaderRegister = RT_REGISTER_SKY_VIEW_SAMPLER;
    samplers[3].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[4] = samplers[1];
    samplers[4].RegisterSpace = RT_REGISTER_SPACE;
    samplers[4].ShaderRegister = RT_REGISTER_CLOUD_SAMPLER;
    samplers[5] = samplers[4];
    samplers[5].ShaderRegister = RT_REGISTER_CLOUD_FIELD_SAMPLER;
    samplers[5].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Renderer::serializeAndCreateRootSignature(params.data(), static_cast<uint32_t>(params.size()),
                                              samplers.data(), static_cast<uint32_t>(samplers.size()), rootSig);
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
    psoDesc.CS = makeShaderBytecode(getShader("cloud_noise_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&cloudNoisePso)));
    psoDesc.CS = makeShaderBytecode(getShader("cloud_shape_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&cloudShapePso)));
    psoDesc.CS = makeShaderBytecode(getShader("cloud_light_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&cloudLightPso)));
    psoDesc.CS = makeShaderBytecode(getShader("cloud_view_cs"));
    CHECK_HRESULT(Renderer::getDevice()->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&cloudViewPso)));
    cloudView.setDimensions(1, 1);
    cloudView.init();
    cloudNoise.setVolumeDimensions(1024, 1, 1024);
    cloudShape.setVolumeDimensions(1024, 1, 1024);
    cloudLight.setVolumeDimensions(128, 32, 128);
    cloudNoise.init();
    cloudShape.init();
    cloudLight.init();

    transmittanceLut.setDimensions(SKY_TRANSMITTANCE_LUT_WIDTH, SKY_TRANSMITTANCE_LUT_HEIGHT);
    transmittanceLut.init();

    multiScatteringLut.setDimensions(SKY_MULTI_SCATTERING_LUT_SIZE, SKY_MULTI_SCATTERING_LUT_SIZE);
    multiScatteringLut.init();

    skyViewLut.setDimensions(SKY_VIEW_LUT_WIDTH, SKY_VIEW_LUT_HEIGHT);
    skyViewLut.init();
}

void dispatch(ID3D12GraphicsCommandList4* cmdList, const float animTime, const float cameraY,
              const bool clouds, const float coverage, const float density, const CloudSettings& settings,
              D3D12_GPU_VIRTUAL_ADDRESS globalParams)
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
        .sunSampleFrame = sunSampleFrame++,
    };
    cmdList->SetComputeRoot32BitConstants(SKY_PARAM_IDX(CONSTANTS), sizeof(SkyConstants) / 4, &constants, 0);
    cmdList->Dispatch(Util::calculateDispatchSize(SKY_VIEW_LUT_WIDTH, SKY_WORKGROUP_SIZE_X),
                      Util::calculateDispatchSize(SKY_VIEW_LUT_HEIGHT, SKY_WORKGROUP_SIZE_Y),
                      1);

    BufferHelper::uavBarrier(cmdList, skyViewLut.getTarget());
    skyViewLut.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (clouds && coverage > 0.f && density > 0.f)
    {
        GPU_PROFILE_SCOPE(cmdList, "cloud fields");
        auto generate = [&](RtTarget& target, ID3D12PipelineState* pso, uint32_t x, uint32_t y, uint32_t z)
        {
            target.transitionToState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->SetPipelineState(pso);
            const SkyConstants cloudConstants = {
                .lutUavIdx = target.getUavIdx(),
                .animTime = animTime,
                .cloudNoiseIdx = cloudNoise.getSrvIdx(),
                .cloudShapeIdx = cloudShape.getSrvIdx(),
                .cloudCoverage = coverage,
                .cloudDensity = density,
                .sunSampleFrame = sunSampleFrame,
                .cloud = settings,
            };
            cmdList->SetComputeRoot32BitConstants(SKY_PARAM_IDX(CONSTANTS), sizeof(SkyConstants) / 4, &cloudConstants, 0);
            cmdList->Dispatch((x + 3) / 4, (y + 3) / 4, (z + 3) / 4);
            BufferHelper::uavBarrier(cmdList, target.getTarget());
            target.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        const bool timeChanged = previousCloudTime != animTime;
        const bool noiseChanged = !cloudNoiseReady || previousCloudSettings.warpScale != settings.warpScale ||
            previousCloudSettings.warpDetail != settings.warpDetail || previousCloudSettings.warpRoughness != settings.warpRoughness ||
            previousCloudSettings.warpTime != settings.warpTime || previousCloudSettings.warpSpeed != settings.warpSpeed ||
            (timeChanged && settings.warpSpeed != 0.f);
        if (noiseChanged)
        {
            generate(cloudNoise, cloudNoisePso.Get(), 1024, 1, 1024);
            cloudNoiseReady = true;
        }
        const bool shapeChanged = noiseChanged || previousCoverage != coverage ||
            std::memcmp(&previousCloudSettings, &settings, sizeof(settings)) != 0 ||
            (timeChanged && settings.voronoiSpeed != 0.f);
        if (shapeChanged)
            generate(cloudShape, cloudShapePso.Get(), 1024, 1, 1024);
        if (shapeChanged || previousDensity != density || previousCloudTime != animTime)
            generate(cloudLight, cloudLightPso.Get(), 128, 32, 128);
        previousCoverage = coverage;
        previousDensity = density;
        previousCloudTime = animTime;
        previousCloudSettings = settings;
        {
            GPU_PROFILE_SCOPE(cmdList, "cloud view");
            cloudView.transitionToState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->SetPipelineState(cloudViewPso.Get());
            cmdList->SetComputeRoot32BitConstant(SKY_PARAM_IDX(CONSTANTS), cloudView.getUavIdx(), 0);
            cmdList->SetComputeRootConstantBufferView(SKY_PARAM_IDX(GLOBAL_PARAMS), globalParams);
            cmdList->Dispatch((cloudViewWidth + 7) / 8, (cloudViewHeight + 7) / 8, 1);
            BufferHelper::uavBarrier(cmdList, cloudView.getTarget());
            cloudView.transitionToState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
}

void resizeCloudView(uint32_t width, uint32_t height, uint32_t divisor)
{
    // Renderer::resize has flushed the GPU before replacing these resources.
    cloudView.reset();
    cloudViewWidth = (width + divisor - 1) / divisor;
    cloudViewHeight = (height + divisor - 1) / divisor;
    cloudView.setDimensions(cloudViewWidth, cloudViewHeight);
    cloudView.init();
}

uint32_t getCloudViewSrvIdx() { return cloudView.getSrvIdx(); }

uint32_t getCloudNoiseSrvIdx() { return cloudNoise.getSrvIdx(); }
uint32_t getCloudShapeSrvIdx() { return cloudShape.getSrvIdx(); }
uint32_t getCloudLightSrvIdx() { return cloudLight.getSrvIdx(); }

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
    cloudNoise.reset(); cloudShape.reset(); cloudLight.reset(); cloudView.reset();
    cloudNoisePso.Reset(); cloudShapePso.Reset(); cloudLightPso.Reset(); cloudViewPso.Reset();
    cloudNoiseReady = false;
    previousCoverage = previousDensity = -1.f;
    previousCloudTime = -1.e20f;
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
