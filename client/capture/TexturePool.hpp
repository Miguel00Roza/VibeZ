#pragma once
#include <vector>
#include <d3d11.h>
#include <memory>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class TexturePool {
private:
    std::vector<ComPtr<ID3D11Texture2D>> textures;

public:
    TexturePool(const ComPtr<ID3D11Device> &device, const D3D11_TEXTURE2D_DESC &desc) {
        textures.resize(3);

        ComPtr<ID3D11Texture2D> texture1;
        ComPtr<ID3D11Texture2D> texture2;
        ComPtr<ID3D11Texture2D> texture3;

        device->CreateTexture2D(&desc, nullptr, texture1.GetAddressOf());
        device->CreateTexture2D(&desc, nullptr, texture2.GetAddressOf());
        device->CreateTexture2D(&desc, nullptr, texture3.GetAddressOf());

        textures[0] = std::move(texture1);
        textures[1] = std::move(texture2);
        textures[2] = std::move(texture3);
    }

    bool GiveTexture(int &textureIndex, ComPtr<ID3D11Texture2D> &texture) {
        for (int i = 0; i < textures.size(); i++) {
            if (textures[i] != nullptr) {
                textureIndex = i;
                texture = std::move(textures[i]);
                return true;
            }
        }
        // Return false when don't have any texture free on pool
        return false;
    }

    void RefoundTexture(const int index, ComPtr<ID3D11Texture2D> &texture) {
        textures[index] = std::move(texture);
    }
};

