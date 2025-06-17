#pragma once

#include "common_structs.hlsli"

cbuffer GlobalParams : register(b0)
{
    CameraParams cameraParams;
    SceneParams sceneParams;
};
