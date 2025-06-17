#include "param_block_manager.h"

#include "dxr_common.h"
#include "buffer/buffer_helper.h"

void ParamBlockManager::init()
{
    constexpr uint32_t bufferSize = sizeof(CameraParams) + sizeof(SceneParams);
    this->dev_paramBuffer =
        BufferHelper::createBasicBuffer(bufferSize, &UPLOAD_HEAP, D3D12_RESOURCE_STATE_GENERIC_READ);
    this->dev_paramBuffer->Map(0, nullptr, &this->host_paramBuffer);

    uint8_t* hostBufferStartPtr = static_cast<uint8_t*>(this->host_paramBuffer);

    this->cameraParams = reinterpret_cast<CameraParams*>(hostBufferStartPtr);
    this->sceneParams = reinterpret_cast<SceneParams*>(this->cameraParams + 1);
}

ID3D12Resource* ParamBlockManager::getDevBuffer() const
{
    return this->dev_paramBuffer.Get();
}
