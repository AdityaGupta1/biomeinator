#pragma once

#include "common_structs.hlsli"

cbuffer GlobalParams : REGISTER_B(REGISTER_GLOBAL_PARAMS)
{
    CameraParams cameraParams;
    SceneParams sceneParams;
};
