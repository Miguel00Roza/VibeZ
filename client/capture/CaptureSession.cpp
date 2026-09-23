#include "CaptureSession.hpp"
#include "FrameQueue.hpp"
#include <spdlog/spdlog.h>
#include <thread>
#include <atomic>
#include <windows.h>
#include <iostream>
#include <queue>
#include <chrono>
#include <condition_variable>


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

    spdlog::error("Failed to found monitor or monitor disconnected");
    return E_FAIL;
}

HRESULT CaptureSession::DuplicateOutput() {

    // Create a D3D11Device and save
    HRESULT hr = D3D11CreateDevice(dxgiAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, id3d11Device.ReleaseAndGetAddressOf(), nullptr, id3d11DeviceContext.ReleaseAndGetAddressOf());
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
    hr = dxgiOutput1->DuplicateOutput(id3d11Device.Get(), dxgiOutputDuplication.ReleaseAndGetAddressOf());
 
    if (FAILED(hr)) {
        spdlog::error("Failed to duplicate IDXGIOutput1, HRESULT: {}", hr);
        return hr;
    }

    return S_OK;
}

HRESULT CaptureSession::CaptureFrame(std::shared_ptr<Frame> &outFrame) {

    // Capture a frame and query as a Texture2d
    while (true) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> dxgiResource{};

        HRESULT hr = dxgiOutputDuplication->AcquireNextFrame(1, &frameInfo, dxgiResource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (lastFrame != nullptr) {
                outFrame = lastFrame;
                return S_OK;
            }
            
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            Sleep(2000);
            // futuramente substituir esse sleep por algo mais correto, isso funciona meio que na gambiarra

            hr = Initialize(monitor);
            if (FAILED(hr)) {
                return hr;
            }

            hr = DuplicateOutput();
            if (FAILED(hr))
                return hr;

            if (texturePool) {
                lastFrame.reset();
                texturePool.reset();
            }
             
            continue;
        }

        if (FAILED(hr)) {
            spdlog::error("Failed to AcquireNextFrame. HRESULT: {}", hr);
            return hr;
        }

        if (frameInfo.LastPresentTime.QuadPart == 0) {
            if (lastFrame != nullptr) {
                dxgiOutputDuplication->ReleaseFrame();
                outFrame = lastFrame;
                return S_OK;
            }
            // Checks if the captured frame is empty, if so, releases it and capture another
            dxgiOutputDuplication->ReleaseFrame();
            continue;
        }

        hr = dxgiResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(id3d11Texture2D.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            dxgiOutputDuplication->ReleaseFrame();

            spdlog::error("Failed to query ID3D11Texture2D");
            return hr;
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

        std::shared_ptr<Frame> frame = std::make_unique<Frame>(frameInfo, texture, texturePool.get(), textureIndex);

        lastFrame = frame;

        outFrame = frame;

        return S_OK;
    }
}


// Producer frame and Consumer frame


std::mutex mtx;
std::condition_variable cv;

void useFrame(FrameQueue* fq, const std::atomic<bool>* sharing) {

    while (sharing->load()) {
        std::unique_lock<std::mutex> lock(mtx);

        cv.wait(lock, [&] { return (!fq->empty()) || (!sharing->load()); });

        std::shared_ptr<Frame> frame = fq->get_front_and_update();
        if (frame) {
            std::cout << "Usou o frame" << std::endl;
        }

        lock.unlock();

        // SIMULA USAR O FRAME OU SLA
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void producerFrames(const std::atomic<bool>* sharing, const double frameRate, CaptureSession* captureSession, FrameQueue* frameQueue) {

    auto inicio = std::chrono::steady_clock::now();
    int fpsCount = 0;

    std::chrono::duration<double> time_per_frame(1.0 / frameRate);

    auto nextDeadline = std::chrono::steady_clock::now();

    while (sharing->load()) {


        nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(time_per_frame);

        std::shared_ptr<Frame> frame;
        HRESULT hr = captureSession->CaptureFrame(frame);

        if (FAILED(hr)) {
            spdlog::error("Error: {}", hr);
            return;
        }

        if (!frame) {
            spdlog::error("Failed to capture frame");
            continue;
        }
        fpsCount++;

        // Scope for lock_guard
        {

            std::lock_guard<std::mutex> lock{ mtx };

            bool r = frameQueue->emplace(frame);
            if (!r) {
                std::cout << "Estourou o limite da queue" << std::endl;
            }

            cv.notify_one();

        }

        std::this_thread::sleep_until(nextDeadline);

        auto duracao = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio);
        if (duracao >= std::chrono::seconds(1)) {
            spdlog::info("FPS: {}", fpsCount);
            inicio = std::chrono::steady_clock::now();
            fpsCount = 0;
        }

    }
}

HRESULT CaptureSession::CaptureScreen(const HMONITOR &monitor, const double frameRate, const std::atomic<bool> *sharing) {
    this->monitor = monitor;

    // Class workflow
    HRESULT hr{};

    hr = Initialize(monitor);
    if (FAILED(hr))
        return hr;

    hr = DuplicateOutput();
    if (FAILED(hr))
        return hr;

    FrameQueue frameQueue{ 3 };
    std::thread t(useFrame, &frameQueue, sharing);
    std::thread t2(producerFrames, sharing, frameRate, this, &frameQueue);

    

    t.join();
    t2.join();

    return S_OK;
}
