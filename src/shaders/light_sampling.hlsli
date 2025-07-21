#pragma once

#include "../rendering/common/common_structs.h"

StructuredBuffer<AreaLight> areaLights : REGISTER_T(REGISTER_AREA_LIGHTS, REGISTER_SPACE_BUFFERS);
StructuredBuffer<uint> areaLightSamplingStructure : REGISTER_T(REGISTER_AREA_LIGHT_SAMPLING_STRUCTURE, REGISTER_SPACE_BUFFERS);