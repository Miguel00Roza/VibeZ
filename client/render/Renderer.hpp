#pragma once
#include <wrl/client.h> // ComPtr
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <spdlog/spdlog.h>

using Microsoft::WRL::ComPtr;

class Renderer {
private:
	HWND hwnd;
	ComPtr<ID3D11Device> device{};
	ComPtr<ID3D11DeviceContext> deviceContext{};
	DXGI_SWAP_CHAIN_DESC swapDesc{};
	ComPtr<IDXGISwapChain> swapChain{};
	ComPtr<ID3D11RenderTargetView> renderTargetView{};

	void CreateRenderTarget() {
		ComPtr<ID3D11Texture2D> backBuffer{};
		swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &backBuffer);
		device->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTargetView.ReleaseAndGetAddressOf());
	}

public:
	Renderer(HWND hwnd, IDXGIAdapter1 *adapter) {
		this->hwnd = hwnd;

		swapDesc.BufferCount = 1;									// backbuffer count
		swapDesc.BufferDesc.Width = 800;							// width
		swapDesc.BufferDesc.Height = 600;							// height
		swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;	// Color format 32-bits
		swapDesc.BufferDesc.RefreshRate.Numerator = 60;				// freshing rate (60hz)
		swapDesc.BufferDesc.RefreshRate.Denominator = 1;			
		swapDesc.OutputWindow = hwnd;								// window handle
		swapDesc.Windowed = TRUE;									// windowed / fullscreen
		swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;		// Buffer used to render target
		swapDesc.SampleDesc.Count = 1;								
		swapDesc.SampleDesc.Quality = 0;
		swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;


		HRESULT hr = D3D11CreateDeviceAndSwapChain(
			adapter, 
			adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 
			nullptr, 
			0, 
			nullptr, 
			0, 
			D3D11_SDK_VERSION, 
			&swapDesc, 
			swapChain.ReleaseAndGetAddressOf(), 
			device.ReleaseAndGetAddressOf(), 
			nullptr, 
			deviceContext.ReleaseAndGetAddressOf()
		);

		if (FAILED(hr)) {
			spdlog::error("Error to create device and swapChain: {}", hr);
			return;
		}

		CreateRenderTarget();
	}

	void Resize(UINT width, UINT height);
	void RenderColor(float r, float g, float b, float a = 1.0f);
};