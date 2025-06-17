#include "param_block_manager.h"

void ParamBlockManager::init()
{
    const uint32_t bufferSize = sizeof(CameraParams) + sizeof(SceneParams);
    this->dev_paramBuffer =
        BufferHelper::createBasicBuffer(bufferSize, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
    this->dev_paramBuffer->Map(0, nullptr, &this->host_paramBuffer);

    this->cameraParams = reinterpret_cast<CameraParams*>(this->host_paramBuffer);
    this->sceneParams =
        reinterpret_cast<SceneParams*>(static_cast<char*>(this->host_paramBuffer) + sizeof(CameraParams));
}

ID3D12Resource* ParamBlockManager::getDevBuffer() const
{
    return this->dev_paramBuffer.Get();
}
