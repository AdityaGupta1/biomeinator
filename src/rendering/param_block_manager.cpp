/*
Biomeinator - real-time path traced voxel engine
Copyright (C) 2025 Aditya Gupta

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

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
