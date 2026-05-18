// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "../rendering/common/common_registers.h"
#include "../rendering/common/common_settings.h"
#include "../rendering/common/common_structs.h"

#include "common/global_params.hlsli"
#include "common/light_tree.hlsli"

StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(RT, AREA_LIGHT_SAMPLING_STRUCTURE);

RWStructuredBuffer<LightAux> lightAuxOut : REGISTER_U(LIGHT_TREE, LIGHT_AUX_OUT);
RWByteAddressBuffer sceneBboxOut : REGISTER_U(LIGHT_TREE, SCENE_BBOX_OUT);
RWStructuredBuffer<uint> mortonKeysOut : REGISTER_U(LIGHT_TREE, MORTON_KEYS_OUT);
RWStructuredBuffer<uint> mortonValuesOut : REGISTER_U(LIGHT_TREE, MORTON_VALUES_OUT);

[shader("compute")]
[numthreads(LIGHT_TREE_MORTON_EMIT_WORKGROUP_SIZE, 1, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint samplingIdx = dispatchThreadId.x;
    if (samplingIdx >= sceneParams.numAreaLights)
    {
        return;
    }

    const uint sparseIdx = areaLightSamplingStructure[samplingIdx];
    const LightAux aux = lightAuxOut[sparseIdx];

    const float3 sceneMin = loadSceneBboxMin(sceneBboxOut);
    const float3 sceneMax = loadSceneBboxMax(sceneBboxOut);

    // 'centroid' is reserved as an HLSL interpolation modifier — use a different name.
    const float3 centerPos = 0.5f * (aux.bboxMin + aux.bboxMax);
    const uint key = mortonEncode30(centerPos, sceneMin, sceneMax);

    mortonKeysOut[samplingIdx] = key;
    mortonValuesOut[samplingIdx] = sparseIdx;
}
