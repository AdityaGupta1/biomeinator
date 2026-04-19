// SPDX-License-Identifier: MIT
// Copyright (c) 2025-2026 Aditya Gupta

#include "param_block_manager.h"

#include "dxr_common.h"
#include "buffer/buffer_helper.h"

static_assert(sizeof(HeapIndices) % 16 == 0, "HeapIndices size must be a multiple of 16 bytes");
static_assert(sizeof(ConstantParams) % 16 == 0, "ConstantParams size must be a multiple of 16 bytes");
static_assert(sizeof(CameraParams) % 16 == 0, "CameraParams size must be a multiple of 16 bytes");
static_assert(sizeof(SceneParams) % 16 == 0, "SceneParams size must be a multiple of 16 bytes");
static_assert(sizeof(RenderParams) % 16 == 0, "RenderParams size must be a multiple of 16 bytes");
static_assert(sizeof(RadianceCacheParams) % 16 == 0, "RadianceCacheParams size must be a multiple of 16 bytes");
static_assert(sizeof(DebugParams) % 16 == 0, "DebugParams size must be a multiple of 16 bytes");

void ParamBlockManager::init()
{
    constexpr uint32_t bufferSize = sizeof(HeapIndices) + sizeof(ConstantParams) + sizeof(CameraParams) +
                                    sizeof(SceneParams) + sizeof(RenderParams) + sizeof(RadianceCacheParams) +
                                    sizeof(DebugParams);
    this->dev_paramBuffer = BufferHelper::createBasicBuffer(bufferSize, &UPLOAD_HEAP);
    this->dev_paramBuffer->Map(0, nullptr, &this->host_paramBuffer);

    uint8_t* hostBufferStartPtr = static_cast<uint8_t*>(this->host_paramBuffer);

    this->heapIndices = reinterpret_cast<HeapIndices*>(hostBufferStartPtr);
    this->constantParams = reinterpret_cast<ConstantParams*>(this->heapIndices + 1);
    this->cameraParams = reinterpret_cast<CameraParams*>(this->constantParams + 1);
    this->sceneParams = reinterpret_cast<SceneParams*>(this->cameraParams + 1);
    this->renderParams = reinterpret_cast<RenderParams*>(this->sceneParams + 1);
    this->rcParams = reinterpret_cast<RadianceCacheParams*>(this->renderParams + 1);
    this->debugParams = reinterpret_cast<DebugParams*>(this->rcParams + 1);
}

void ParamBlockManager::reset()
{
    this->dev_paramBuffer.Reset();
}

ID3D12Resource* ParamBlockManager::getDevBuffer() const
{
    return this->dev_paramBuffer.Get();
}

void ParamBlockManager::setName(const std::wstring& name)
{
    this->dev_paramBuffer->SetName(name.c_str());
}
