// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "renderer_internal.h"

#include <d3dcompiler.h>

#include "pipeline_builder.h"
#include "shaders.h"
#include "rendering/common/common_hitgroups.h"

namespace Renderer
{

static constexpr uint32_t maxPayloadSizeBytes = 96;

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
    if (voxelMode)
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

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC gbufferRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(gbufferParams.size()),
                .pParameters = gbufferParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(rtStaticSamplers.size()),
                .pStaticSamplers = rtStaticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&gbufferRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gbufferRootSig)));
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

        if (useSer)
        {
            ptParams.push_back({
                .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                .DescriptorTable = {
                    .NumDescriptorRanges = 1,
                    .pDescriptorRanges = &serDescriptorRange,
                },
            });
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rtRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(ptParams.size()),
                .pParameters = ptParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(rtStaticSamplers.size()),
                .pStaticSamplers = rtStaticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&rtRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(
            device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&ptRootSig)));
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, COLLECT_PARAM_IDX(COUNT)> collectParams;

        collectParams[COLLECT_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);
        collectParams[COLLECT_PARAM_IDX(PATH_TRACING_RAW_BUFFER_IN)] = MAKE_PARAM(SRV, COLLECT, PATH_TRACING_RAW_BUFFER_IN);
        collectParams[COLLECT_PARAM_IDX(PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN)] = MAKE_PARAM(SRV, COLLECT, PT_DIFFUSE_ALBEDO_RAW_BUFFER_IN);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC collectRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(collectParams.size()),
                .pParameters = collectParams.data(),
                .NumStaticSamplers = 0,
                .pStaticSamplers = nullptr,
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&collectRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&collectRootSig)));
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

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC nrcResolveRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(nrcResolveParams.size()),
                .pParameters = nrcResolveParams.data(),
                .NumStaticSamplers = 0,
                .pStaticSamplers = nullptr,
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&nrcResolveRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&nrcResolveRootSig)));
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

        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        staticSamplers.push_back(postprocessSamplerDesc);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC postprocessRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(postprocessParams.size()),
                .pParameters = postprocessParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(staticSamplers.size()),
                .pStaticSamplers = staticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&postprocessRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&postprocessRootSig)));
    }

    // ===================================
    // DEBUG VIEW
    // ===================================
    {
        std::array<D3D12_ROOT_PARAMETER1, DEBUG_VIEW_PARAM_IDX(COUNT)> debugViewParams;

        debugViewParams[DEBUG_VIEW_PARAM_IDX(GLOBAL_PARAMS)] = MAKE_PARAM(CBV, COMMON, GLOBAL_PARAMS);

        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        staticSamplers.push_back(postprocessSamplerDesc);

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC debugViewRootSigDesc = {
            .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
            .Desc_1_1 = {
                .NumParameters = static_cast<uint32_t>(debugViewParams.size()),
                .pParameters = debugViewParams.data(),
                .NumStaticSamplers = static_cast<uint32_t>(staticSamplers.size()),
                .pStaticSamplers = staticSamplers.data(),
                .Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED,
            },
        };

        ComPtr<ID3DBlob> blob, errorBlob;
        CHECK_HRESULT_WITH_ERROR_BLOB(D3D12SerializeVersionedRootSignature(&debugViewRootSigDesc, &blob, &errorBlob),
                                      errorBlob);
        CHECK_HRESULT(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&debugViewRootSig)));
    }
}

void initPipeline()
{
    // ===================================
    // GBUFFER
    // ===================================
    {
        RtPipelineInputs gbufferPipelineInputs = {
            .name = L"gbuffer",
            .pso = gbufferPso,
            .dev_shaderIds = dev_gbufferShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = gbufferDispatchDesc,
        };

        gbufferPipelineInputs.shaderBytecode = getShader("gbuffer_rgs");
        gbufferPipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        gbufferPipelineInputs.rootSig = gbufferRootSig.Get();

        gbufferPipelineInputs.hitGroups.resize(2);
        gbufferPipelineInputs.hitGroups[GBUFFER_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"gbuffer_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        gbufferPipelineInputs.hitGroups[GBUFFER_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"gbuffer_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };

        makeRtPipeline(gbufferPipelineInputs);
    }

    // ===================================
    // NRC UPDATE
    // ===================================
    {
        RtPipelineInputs nrcUpdatePipelineInputs = {
            .name = L"nrcUpdate",
            .pso = nrcUpdatePso,
            .dev_shaderIds = dev_nrcUpdateShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = nrcUpdateDispatchDesc,
        };

        nrcUpdatePipelineInputs.shaderBytecode = getShader("nrc_update_rgs");
        nrcUpdatePipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        nrcUpdatePipelineInputs.rootSig = ptRootSig.Get();

        nrcUpdatePipelineInputs.hitGroups.resize(3);
        nrcUpdatePipelineInputs.hitGroups[PT_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"nrcUpdate_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        nrcUpdatePipelineInputs.hitGroups[PT_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"nrcUpdate_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };
        nrcUpdatePipelineInputs.hitGroups[PT_HITGROUP_DOME_LIGHT] = {
            .HitGroupExport = L"nrcUpdate_HitGroup_DomeLight",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_DomeLight",
        };

        makeRtPipeline(nrcUpdatePipelineInputs);
    }

    // ===================================
    // NRC QUERY
    // ===================================
    {
        RtPipelineInputs nrcQueryPipelineInputs = {
            .name = L"nrcQuery",
            .pso = nrcQueryPso,
            .dev_shaderIds = dev_nrcQueryShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = nrcQueryDispatchDesc,
        };

        nrcQueryPipelineInputs.shaderBytecode = getShader("nrc_query_rgs");
        nrcQueryPipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        nrcQueryPipelineInputs.rootSig = ptRootSig.Get();

        nrcQueryPipelineInputs.hitGroups.resize(3);
        nrcQueryPipelineInputs.hitGroups[PT_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"nrcQuery_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        nrcQueryPipelineInputs.hitGroups[PT_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"nrcQuery_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };
        nrcQueryPipelineInputs.hitGroups[PT_HITGROUP_DOME_LIGHT] = {
            .HitGroupExport = L"nrcQuery_HitGroup_DomeLight",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_DomeLight",
        };

        makeRtPipeline(nrcQueryPipelineInputs);
    }

    // ===================================
    // PATH TRACING
    // ===================================
    {
        RtPipelineInputs ptPipelineInputs = {
            .name = L"pathTracing",
            .pso = ptPso,
            .dev_shaderIds = dev_ptShaderIds,
            .rgsShaderName = L"RayGeneration",
            .missShaderName = L"Miss",
            .dispatchDesc = ptDispatchDesc,
        };

        ptPipelineInputs.shaderBytecode = getShader("path_tracing_rgs");
        ptPipelineInputs.maxPayloadSizeBytes = maxPayloadSizeBytes;
        ptPipelineInputs.rootSig = ptRootSig.Get();

        ptPipelineInputs.hitGroups.resize(3);
        ptPipelineInputs.hitGroups[PT_HITGROUP_PRIMARY] = {
            .HitGroupExport = L"pt_HitGroup_Primary",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Primary",
        };
        ptPipelineInputs.hitGroups[PT_HITGROUP_LIGHTS] = {
            .HitGroupExport = L"pt_HitGroup_Lights",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_Lights",
        };
        ptPipelineInputs.hitGroups[PT_HITGROUP_DOME_LIGHT] = {
            .HitGroupExport = L"pt_HitGroup_DomeLight",
            .Type = D3D12_HIT_GROUP_TYPE_TRIANGLES,
            .AnyHitShaderImport = L"AnyHit",
            .ClosestHitShaderImport = L"ClosestHit_DomeLight",
        };

        makeRtPipeline(ptPipelineInputs);
    }

    // ===================================
    // COLLECT
    // ===================================
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = collectRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("collect_cs"));
        CHECK_HRESULT(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&collectPso)));
        collectPso->SetName(L"collectPso");
    }

    // ===================================
    // NRC RESOLVE
    // ===================================
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = nrcResolveRootSig.Get();
        psoDesc.CS = makeShaderBytecode(getShader("nrc_resolve_cs"));
        CHECK_HRESULT(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&nrcResolvePso)));
        nrcResolvePso->SetName(L"nrcResolvePso");
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
            psoDesc.pRootSignature = postprocessRootSig.Get();
            psoDesc.VS = postprocessVsShaderBytecode;
            psoDesc.PS = makeShaderBytecode(getShader("postprocess_ps"));
            CHECK_HRESULT(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&postprocessPso)));
            postprocessPso->SetName(L"postprocessPso");
        }

        // ===================================
        // DEBUG VIEW
        // ===================================
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = postprocessPsoDescBase;
            psoDesc.pRootSignature = debugViewRootSig.Get();
            psoDesc.VS = postprocessVsShaderBytecode;
            psoDesc.PS = makeShaderBytecode(getShader("debug_view_ps"));
            CHECK_HRESULT(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&debugViewPso)));
            debugViewPso->SetName(L"debugViewPso");
        }
    }
}

} // namespace Renderer
