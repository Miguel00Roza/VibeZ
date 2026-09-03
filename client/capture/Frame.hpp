#pragma once
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <spdlog/spdlog.h>

#include "TexturePool.hpp"


using Microsoft::WRL::ComPtr;

class Frame {
public:
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<ID3D11Texture2D> frameId3d11Texture2D{};
    TexturePool* texturePool;
    int textureIndex;

    Frame(const DXGI_OUTDUPL_FRAME_INFO &frameInfo, ComPtr<ID3D11Texture2D> frameId3d11Texture2D, TexturePool* pool, const int index) {
        this->frameInfo = frameInfo;
        this->frameId3d11Texture2D = std::move(frameId3d11Texture2D);
        this->textureIndex = index;
        this->texturePool = pool;
    }

    ~Frame() {
        texturePool->RefoundTexture(textureIndex, frameId3d11Texture2D);
    };
};



