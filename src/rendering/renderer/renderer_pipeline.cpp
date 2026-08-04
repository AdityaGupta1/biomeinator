// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <d3dcompiler.h>

#include "pipeline_builder.h"
#include "shaders.h"
#include "rendering/common/common_hitgroups.h"

namespace Renderer
{

static constexpr uint32_t maxPayloadSizeBytes = 88;

void serializeAndCreateRootSignature(const D3D12_ROOT_PARAMETER1* params,
                                     uint32_t numParams,
                                     const D3D12_STATIC_SAMPLER_DESC* staticSamplers,
                                     uint32_t numStaticSamplers,
                                     ComPtr<ID3D12RootSignature>& outRootSig)
{
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {
        .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
        .Desc_1_1 = {
            .NumParameters = numParams,
            .pParameters = params,
            .NumStaticSamplers = numStaticSamplers,
            .pStaticSamplers = staticSamplers,
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
        },
    };

    ComPtr<ID3DBlob> blob, errorBlob;
    CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&desc, &blob, &errorBlob), errorBlob);
    CHECK_HRESULT(renderState.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&outRootSig)));
}

void initRootSignature()
{
    std::vector<D3D12_STATIC_SAMPLER_DESC> rtStaticSamplers;

    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MinLOD = 0.f;
    staticSampler.ShaderRegister = RT_REGISTER_TEX_SAMPLER;
    staticSampler.RegisterSpace = RT_REGISTER_SPACE;
    if (renderState.voxelMode)
    {
        staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        staticSampler.MaxLOD = 4.f;
    }
    else
    {
        staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSampler.MaxLOD = 0.f;
    }

    rtStaticSamplers.push_back(staticSampler);

    // Linear clamp sampler for sky atmosphere LUTs
    const D3D12_STATIC_SAMPLER_DESC lutSampler = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ShaderRegister = RT_REGISTER_LUT_SAMPLER,
        .RegisterSpace = RT_REGISTER_SPACE,
    };
    rtStaticSamplers.push_back(lutSampler);

    // The sky-view LUT is periodic in azimuth, so bilinear filtering must wrap across the
    // u = 0/1 seam; latitude (v) still clamps
    const D3D12_STATIC_SAMPLER_DESC skyViewSampler = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ShaderRegister = RT_REGISTER_SKY_VIEW_SAMPLER,
        .RegisterSpace = RT_REGISTER_SPACE,
    };
    rtStaticSamplers.push_back(skyViewSampler);

    // Linear clamp sampler for the world-XZ biome color map
    const D3D12_STATIC_SAMPLER_DESC biomeMapSampler = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .ShaderRegister = RT_REGISTER_BIOME_MAP_SAMPLER,
        .RegisterSpace = RT_REGISTER_SPACE,
    };
    rtStaticSamplers.push_back(biomeMapSampler);

    const D3D12_DESCRIPTOR_RANGE1 serDescriptorRange = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1,
        .BaseShaderRegister = NV_SHADER_EXTN_SLOT,
        .RegisterSpace = NV_SHADER_EXTN_REGISTER_SPACE,
        .Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
        .OffsetInDescriptorsFromTableStart = 0,
    };

    // ===================================
    // GBUFFER
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, GBUFFER_PARAM_IDX(COUNT)> gbufferParams;

        gbufferParams[GBUFFER_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        gbufferParams[GBUFFER_PARAM_IDX(RAYTRACING_ACS)] = MAKE_PARAM(SRV, RT, RAYTRACING_ACS);
        gbufferParams[GBUFFER_PARAM_IDX(VERTS)] = MAKE_PARAM(SRV, RT, VERTS);
        gbufferParams[GBUFFER_PARAM_IDX(IDXS)] = MAKE_PARAM(SRV, RT, IDXS);
        gbufferParams[GBUFFER_PARAM_IDX(INSTANCE_DATAS)] = MAKE_PARAM(SRV, RT, INSTANCE_DATAS);
        gbufferParams[GBUFFER_PARAM_IDX(MATERIALS)] = MAKE_PARAM(SRV, RT, MATERIALS);
        gbufferParams[GBUFFER_PARAM_IDX(PER_TRI_DATAS)] = MAKE_PARAM(SRV, RT, PER_TRI_DATAS);
        gbufferParams[GBUFFER_PARAM_IDX(AREA_LIGHTS)] = MAKE_PARAM(SRV, RT, AREA_LIGHTS);
        gbufferParams[GBUFFER_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, RT, AREA_LIGHT_SAMPLING_STRUCTURE);

        gbufferParams[GBUFFER_PARAM_IDX(GBUFFER_OUT)] = MAKE_PARAM(UAV, GBUFFER, GBUFFER_OUT);

        serializeAndCreateRootSignature(gbufferParams.data(), static_cast<uint32_t>(gbufferParams.size()),
                                        rtStaticSamplers.data(), static_cast<uint32_t>(rtStaticSamplers.size()),
                                        renderState.gbufferRootSig);
    }

    // ===================================
    // PATH TRACING
    // ===================================
    {
        std::vector<D3D12_ROOT_PARAMETER1> ptParams;
        ptParams.resize(PT_PARAM_IDX(COUNT));

        ptParams[PT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        ptParams[PT_PARAM_IDX(RAYTRACING_ACS)] = MAKE_PARAM(SRV, RT, RAYTRACING_ACS);
        ptParams[PT_PARAM_IDX(VERTS)] = MAKE_PARAM(SRV, RT, VERTS);
        ptParams[PT_PARAM_IDX(IDXS)] = MAKE_PARAM(SRV, RT, IDXS);
        ptParams[PT_PARAM_IDX(INSTANCE_DATAS)] = MAKE_PARAM(SRV, RT, INSTANCE_DATAS);
        ptParams[PT_PARAM_IDX(MATERIALS)] = MAKE_PARAM(SRV, RT, MATERIALS);
        ptParams[PT_PARAM_IDX(PER_TRI_DATAS)] = MAKE_PARAM(SRV, RT, PER_TRI_DATAS);
        ptParams[PT_PARAM_IDX(AREA_LIGHTS)] = MAKE_PARAM(SRV, RT, AREA_LIGHTS);
        ptParams[PT_PARAM_IDX(AREA_LIGHT_SAMPLING_STRUCTURE)] = MAKE_PARAM(SRV, RT, AREA_LIGHT_SAMPLING_STRUCTURE);

        ptParams[PT_PARAM_IDX(GBUFFER_IN)] = MAKE_PARAM(SRV, PT, GBUFFER_IN);

        ptParams[PT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT)] = MAKE_PARAM(UAV, PT, PATH_TRACING_RAW_BUFFER_OUT);
        ptParams[PT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT)] = MAKE_PARAM(UAV, PT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_OUT);

        ptParams[PT_PARAM_IDX(NRC_CONSTANTS)] = MAKE_PARAM(CBV, NRC, NRC_CONSTANTS);

        ptParams[PT_PARAM_IDX(NRC_QUERY_PATH_INFO)] = MAKE_PARAM(UAV, NRC, QUERY_PATH_INFO);
        ptParams[PT_PARAM_IDX(NRC_TRAINING_PATH_INFO)] = MAKE_PARAM(UAV, NRC, TRAINING_PATH_INFO);
        ptParams[PT_PARAM_IDX(NRC_TRAINING_PATH_VERTICES)] = MAKE_PARAM(UAV, NRC, TRAINING_PATH_VERTICES);
        ptParams[PT_PARAM_IDX(NRC_QUERY_RADIANCE_PARAMS)] = MAKE_PARAM(UAV, NRC, QUERY_RADIANCE_PARAMS);
        ptParams[PT_PARAM_IDX(NRC_COUNTERS_DATA)] = MAKE_PARAM(UAV, NRC, COUNTERS_DATA);

        ptParams[PT_PARAM_IDX(RTSL_LIGHT_TREE)] = MAKE_PARAM(SRV, LIGHT_TREE, LIGHT_TREE_IN);
        ptParams[PT_PARAM_IDX(RTSL_LIGHT_TO_LEAF)] = MAKE_PARAM(SRV, LIGHT_TREE, LIGHT_TO_LEAF_IN);

        if (renderState.useSer)
        {
            ptParams.push_back({
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = {
                    .NumDescriptorRanges = 1,
                    .pDescriptorRanges = &serDescriptorRange,
                },
            });
        }

        serializeAndCreateRootSignature(ptParams.data(), static_cast<uint32_t>(ptParams.size()),
                                        rtStaticSamplers.data(), static_cast<uint32_t>(rtStaticSamplers.size()),
                                        renderState.ptRootSig);
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, COLLECT_PARAM_IDX(COUNT)> collectParams;

        collectParams[COLLECT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);
        collectParams[COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN)] = MAKE_PARAM(SRV, COLLECT, PATH_TRACING_RAW_BUFFER_IN);
        collectParams[COLLECT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN)] = MAKE_PARAM(SRV, COLLECT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN);

        serializeAndCreateRootSignature(collectParams.data(), static_cast<uint32_t>(collectParams.size()),
                                        nullptr, 0, renderState.collectRootSig);
    }

    // ===================================
    // NRC RESOLVE
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, NRC_RESOLVE_PARAM_IDX(COUNT)> nrcResolveParams;

        nrcResolveParams[NRC_RESOLVE_PARAM_IDX(NRC_CONSTANTS)] = MAKE_PARAM(CBV, NRC, NRC_CONSTANTS);
        nrcResolveParams[NRC_RESOLVE_PARAM_IDX(QUERY_PATH_INFO)] = MAKE_PARAM(UAV, NRC, QUERY_PATH_INFO);
        nrcResolveParams[NRC_RESOLVE_PARAM_IDX(QUERY_RADIANCE)] = MAKE_PARAM(UAV, NRC, QUERY_RADIANCE);
        nrcResolveParams[NRC_RESOLVE_PARAM_IDX(PATH_TRACING_RAW_BUFFER_OUT)] = MAKE_PARAM(UAV, PT, PATH_TRACING_RAW_BUFFER_OUT);

        serializeAndCreateRootSignature(nrcResolveParams.data(), static_cast<uint32_t>(nrcResolveParams.size()),
                                        nullptr, 0, renderState.nrcResolveRootSig);
    }

    // ===================================
    // POSTPROCESSING
    // ===================================
    const D3D12_STATIC_SAMPLER_DESC postprocessSamplerDesc = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .ShaderRegister = POSTPROCESS_REGISTER_TEX_SAMPLER,
        .RegisterSpace = POSTPROCESS_REGISTER_SPACE,
    };

    {
        std::array<D3D12_ROOT_PARAMETER1, POSTPROCESS_PARAM_IDX(COUNT)> postprocessParams;

        postprocessParams[POSTPROCESS_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        serializeAndCreateRootSignature(postprocessParams.data(), static_cast<uint32_t>(postprocessParams.size()),
                                        &postprocessSamplerDesc, 1, renderState.postprocessRootSig);
    }

    // ===================================
    // DEBUG VIEW
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, DEBUG_VIEW_PARAM_IDX(COUNT)> debugViewParams;

        debugViewParams[DEBUG_VIEW_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        serializeAndCreateRootSignature(debugViewParams.data(), static_cast<uint32_t>(debugViewParams.size()),
                                        &postprocessSamplerDesc, 1, renderState.debugViewRootSig);
    }
}

void initPipeline()
{
    // ===================================
    // RT PIPELINES
    // ===================================
    {
        const auto makeCommonRtPipeline = [](const std::wstring& name, const char* rgsShader,
                                             ID3D12RootSignature* rootSig, ComPtr<ID3D12StateObject>& pso,
                                             ComPtr<ID3D12Resource>& dev_shaderIds, D3D12_DISPATCH_RAYS_DESC& dispatchDesc)
        {
            RtPipelineInputs pipelineInputs = {
                .name = name,
                .pso = pso,
                .dev_shaderIds = dev_shaderIds,
                .rgsShaderName = L"RayGeneration",
                .missShaderName = L"Miss",
                .dispatchDesc = dispatchDesc,
            };

            pipelineInputs.shaderBytecode = getShader(rgsShader);
            pipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
            pipelineInputs.rootSig = rootSig;

            const std::wstring primaryHitGroupName = name + L"_HitGroup_Primary";
            const std::wstring lightsHitGroupName = name + L"_HitGroup_Lights";

            pipelineInputs.hitGroups.resize(2);
            pipelineInputs.hitGroups[HITGROUP_PRIMARY] = {
                .HitGroupExport = primaryHitGroupName.c_str(),
                .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
                .AnyHitShaderImport = L"AnyHit",
                .ClosestHitShaderImport = L"ClosestHit_Primary",
            };
            // No CHS needed for shadow rays
            pipelineInputs.hitGroups[HITGROUP_LIGHTS] = {
                .HitGroupExport = lightsHitGroupName.c_str(),
                .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
                .AnyHitShaderImport = L"AnyHit",
            };

            makeRtPipeline(pipelineInputs);
        };

        makeCommonRtPipeline(L"gbuffer", "gbuffer_rgs", renderState.gbufferRootSig.Get(),
                             renderState.gbufferPso, renderState.dev_gbufferShaderIds, renderState.gbufferDispatchDesc);
        makeCommonRtPipeline(L"nrcUpdate", "nrc_update_rgs", renderState.ptRootSig.Get(),
                             renderState.nrcUpdatePso, renderState.dev_nrcUpdateShaderIds, renderState.nrcUpdateDispatchDesc);
        makeCommonRtPipeline(L"nrcQuery", "nrc_query_rgs", renderState.ptRootSig.Get(),
                             renderState.nrcQueryPso, renderState.dev_nrcQueryShaderIds, renderState.nrcQueryDispatchDesc);
        makeCommonRtPipeline(L"pathTracing", "path_tracing_rgs", renderState.ptRootSig.Get(),
                             renderState.ptPso, renderState.dev_ptShaderIds, renderState.ptDispatchDesc);
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = renderState.collectRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("collect_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&renderState.collectPso)));
        renderState.collectPso->SetName(L"collectPso");
    }

    // ===================================
    // NRC RESOLVE
    // ===================================
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = renderState.nrcResolveRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("nrc_resolve_cs"));
        CHECK_HRESULT(renderState.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&renderState.nrcResolvePso)));
        renderState.nrcResolvePso->SetName(L"nrcResolvePso");
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC postprocessPsoDescBase{};
        postprocessPsoDescBase.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        postprocessPsoDescBase.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        postprocessPsoDescBase.DepthStencilState = {
            .DepthEnable = FALSE,
            .StencilEnable = FALSE,
        };
        postprocessPsoDescBase.SampleMask = UINT_MAX;
        postprocessPsoDescBase.InputLayout = { nullptr, 0 }; // no verts/idxs
        postprocessPsoDescBase.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        postprocessPsoDescBase.NumRenderTargets = 1;
        postprocessPsoDescBase.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        postprocessPsoDescBase.SampleDesc = SAMPLE_DESC_NO_AA;

        const D3D12_SHADER_BYTECODE postprocessVsShaderBytecode = makeShaderBytecode(getShader("postprocess_vs"));

        // ===================================
        // POSTPROCESSING
        // ===================================
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = postprocessPsoDescBase;
            psoDesc.pRootSignature = renderState.postprocessRootSig.Get();
            psoDesc.VS = postprocessVsShaderBytecode;
            psoDesc.PS = makeShaderBytecode(getShader("postprocess_ps"));
            CHECK_HRESULT(renderState.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderState.postprocessPso)));
            renderState.postprocessPso->SetName(L"postprocessPso");
        }

        // ===================================
        // DEBUG VIEW
        // ===================================
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = postprocessPsoDescBase;
            psoDesc.pRootSignature = renderState.debugViewRootSig.Get();
            psoDesc.VS = postprocessVsShaderBytecode;
            psoDesc.PS = makeShaderBytecode(getShader("debug_view_ps"));
            CHECK_HRESULT(renderState.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderState.debugViewPso)));
            renderState.debugViewPso->SetName(L"debugViewPso");
        }
    }
}

} // namespace Renderer
