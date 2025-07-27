#pragma once

#include "../rendering/common/common_structs.h"
#include "../rendering/common/common_registers.h"

cbuffer GlobalParams : REGISTER_B(REGISTER_GLOBAL_PARAMS, REGISTER_SPACE_BUFFERS)
{
    CameraParams cameraParams;
    SceneParams sceneParams;
};
