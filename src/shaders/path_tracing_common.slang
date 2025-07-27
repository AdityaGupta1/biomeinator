#pragma once

#include "../rendering/common/common_structs.h"

RaytracingAccelerationStructure raytracingAcs : REGISTER_T(REGISTER_RAYTRACING_ACS, REGISTER_SPACE_BUFFERS);

StructuredBuffer<InstanceData> instanceDatas : REGISTER_T(REGISTER_INSTANCE_DATAS, REGISTER_SPACE_BUFFERS);

StructuredBuffer<Material> materials : REGISTER_T(REGISTER_MATERIALS, REGISTER_SPACE_BUFFERS);
