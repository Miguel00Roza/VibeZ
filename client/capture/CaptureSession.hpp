#pragma once
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <memory>
#include <atomic>

#include "Frame.hpp"
#include "TexturePool.hpp"

using Microsoft::WRL::ComPtr;

class CaptureSession {
private:
    HMONITOR monitor{};
    ComPtr<IDXGIAdapter1> dxgiAdapter{};
    ComPtr<IDXGIOutput> dxgiOutput{};
    ComPtr<ID3D11Device> id3d11Device{};
    ComPtr<ID3D11DeviceContext> id3d11DeviceContext{};
    ComPtr<IDXGIOutputDuplication> dxgiOutputDuplication{};
    ComPtr<ID3D11Resource> id3d11Resource{};
    ComPtr<ID3D11Texture2D> id3d11Texture2D{};
    std::unique_ptr<TexturePool> texturePool{};
    std::shared_ptr<Frame> lastFrame = nullptr;

public:
    HRESULT Initialize(const HMONITOR &monitor);
    HRESULT ConsultMonitor();
    HRESULT DuplicateOutput();
    HRESULT CaptureFrame(std::shared_ptr<Frame> &outFrame);
    HRESULT CaptureScreen(const HMONITOR &monitor, const double frameRate, const std::atomic<bool> *sharing);
};


