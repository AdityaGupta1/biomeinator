// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "util/math.hlsli"

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(RT, INSTANCE_DATAS);
StructuredBuffer<Material> materials : REGISTER_T(RT, MATERIALS);
StructuredBuffer<AreaLight> areaLights : REGISTER_T(RT, AREA_LIGHTS);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(RT, AREA_LIGHT_SAMPLING_STRUCTURE);

// Both UAVs are keyed by the SPARSE areaLights[] index (matches what Stage 4's
// BSDF-hit recovery computes from instanceDatas[...].areaLightsBufferOffset +
// perTriDatas[...].localAreaLightIdx).
RWStructuredBuffer<LightAux> lightAuxOut : REGISTER_U(LIGHT_TREE, LIGHT_AUX_OUT);
RWStructuredBuffer<uint> lightToLeafOut : REGISTER_U(LIGHT_TREE, LIGHT_TO_LEAF_OUT);

[shader("compute")]
[numthreads(EMITTER_COLLECT_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint samplingIdx = dispatchThreadId.x;
    if (samplingIdx >= sceneParams.numAreaLights)
    {
        return;
    }

    const uint areaLightIdx = areaLightSamplingStructure[samplingIdx];
    const AreaLight light = areaLights[areaLightIdx];

    const InstanceData instanceData = instanceDatas[light.instanceId];
    const float3 instanceOffset = float3(instanceData.transformOffset) - float3(cameraParams.globalInstanceOffset);

    const float3 p0 = light.pos0_WS + instanceOffset;
    const float3 p1 = light.pos1_WS + instanceOffset;
    const float3 p2 = light.pos2_WS + instanceOffset;

    const float3 bboxMin = min(p0, min(p1, p2));
    const float3 bboxMax = max(p0, max(p1, p2));

    const float triangleArea = 0.5f * length(cross(p1 - p0, p2 - p0));

    float flux = 0.f;
    if (light.materialIdx != MATERIAL_IDX_INVALID)
    {
        const Material material = materials[light.materialIdx];
        const float colorTerm = (material.emissiveColorTextureId == TEXTURE_ID_INVALID)
            ? luminance(material.emissiveColor)
            : 1.f;
        flux = material.emissiveStrength * colorTerm * triangleArea;
    }

    LightAux aux;
    aux.bboxMin = bboxMin;
    aux.flux = flux;
    aux.bboxMax = bboxMax;
    aux.pad0 = 0; // DXC validator rejects struct UAV stores with undef members
    lightAuxOut[areaLightIdx] = aux;

    // lightToLeaf is left at LEAF_IDX_INVALID by light_buffer_clear; Stage 2
    // scatters the real leaf index for live slots after the sort.
}
