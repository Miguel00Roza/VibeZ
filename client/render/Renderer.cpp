#include "Renderer.hpp"
#include <wrl/client.h> // ComPtr
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <spdlog/spdlog.h>

void Renderer::Resize(UINT width, UINT height) {
	if (!swapChain)
		return;

	renderTargetView.Reset();

	swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

	CreateRenderTarget();
}

void Renderer::RenderColor(float r, float g, float b, float a) {
	float clearColor[4] = { r, g, b, a };

	deviceContext->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);
	deviceContext->ClearRenderTargetView(renderTargetView.Get(), clearColor);

	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(swapDesc.BufferDesc.Width);
	viewport.Height = static_cast<float>(swapDesc.BufferDesc.Height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	deviceContext->RSSetViewports(1, &viewport);

	swapChain->Present(1, 0);
}