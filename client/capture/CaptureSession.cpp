#include "CaptureSession.hpp"
#include <spdlog/spdlog.h>

#include <chrono>

HRESULT CaptureSession::Initialize(const HMONITOR &monitor) {

    // Create a factory just to enumerate adapters
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::error("Failed to create DXGIFactory");
        return hr;
    }


    // Save the output and adapter based on selected monitor and exit
    UINT i = 0;
    UINT j = 0;
    while (factory->EnumAdapters1(i, dxgiAdapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        j = 0;
        while (dxgiAdapter->EnumOutputs(j, dxgiOutput.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC outputDesc;
            dxgiOutput->GetDesc(&outputDesc);
            if (outputDesc.Monitor == monitor) {
                return S_OK;
            }
            ++j;
        }
        ++i;
    }

    return E_FAIL;
}

HRESULT CaptureSession::DuplicateOutput() {

    // Create a D3D11Device and save
    HRESULT hr = D3D11CreateDevice(dxgiAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, id3d11Device.GetAddressOf(), nullptr, id3d11DeviceContext.GetAddressOf());
    if (FAILED(hr)) {
        spdlog::error("Failed to create D3D11 device");
        return hr;
    }


    // Create a IDXGIOutput1 just to create a IDXGIOutputDuplication
    ComPtr<IDXGIOutput1> dxgiOutput1;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(dxgiOutput1.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::error("Failed to query IDXGIOutput1");
        return hr;
    }


    // Duplicate DXGIOutput1 and save
    hr = dxgiOutput1->DuplicateOutput(id3d11Device.Get(), dxgiOutputDuplication.GetAddressOf());
    if (FAILED(hr)) {
        spdlog::error("Failed to duplicate IDXGIOutput1");
        return hr;
    }

    return S_OK;
}

std::unique_ptr<Frame> CaptureSession::CaptureFrame() {

    // Capture a frame and query as a Texture2d
    while (true) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> dxgiResource{};

        HRESULT hr = dxgiOutputDuplication->AcquireNextFrame(2000, &frameInfo, dxgiResource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }

        if (FAILED(hr)) {
            spdlog::error("Failed to AcquireNextFrame. HRESULT: {}", hr);
            return {};
        }

        if (frameInfo.LastPresentTime.QuadPart == 0) {
            // Checks if the captured frame is empty, if so, releases it and capture another
            dxgiOutputDuplication->ReleaseFrame();
            continue;
        }

        hr = dxgiResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(id3d11Texture2D.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            dxgiOutputDuplication->ReleaseFrame();

            spdlog::error("Failed to query ID3D11Texture2D");
            return {};
        }

        // initialize texturePool if it hasn't already happened
        if (!texturePool) {
            D3D11_TEXTURE2D_DESC desc{};
            id3d11Texture2D->GetDesc(&desc);
            texturePool = std::make_unique<TexturePool>(id3d11Device, desc);
        }

        ComPtr<ID3D11Texture2D> texture{};
        int textureIndex;

        // Checks if a texture is available; if not, discards the frame.
        if (!(texturePool->GiveTexture(textureIndex, texture))) {
            dxgiOutputDuplication->ReleaseFrame();
            continue;
        }

        id3d11DeviceContext->CopyResource(texture.Get(), id3d11Texture2D.Get());

        dxgiOutputDuplication->ReleaseFrame();

        std::unique_ptr<Frame> frame = std::make_unique<Frame>(frameInfo, texture, texturePool.get(), textureIndex);

        return frame;
    }
}

HRESULT CaptureSession::CaptureScreen(const HMONITOR &monitor) {
    // Class workflow
    HRESULT hr{};

    hr = Initialize(monitor);
    if (FAILED(hr))
        return hr;

    hr = DuplicateOutput();
    if (FAILED(hr))
        return hr;

    auto inicio = std::chrono::steady_clock::now();
    int fpsCount = 0;

    bool sharing = true;
    while (sharing) {
        auto frame = CaptureFrame();
        if (!frame) {
            spdlog::error("Failed to capture frame");
        }
        fpsCount += 1;

        auto duracao = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio);
        if (duracao >= std::chrono::seconds(1)) {
            spdlog::info("FPS: {}", fpsCount);
            inicio = std::chrono::steady_clock::now();
            fpsCount = 0;
        }

    }

    return S_OK;
}
