#include <windows.h>

#include "client/render/Renderer.hpp"
#include "client/window/Window.hpp"

int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {

	Window window(hInstance, nCmdShow);

	window.Show();
	window.Update();

	Renderer renderer(window.getHwnd(), nullptr);

	MSG msg = {};
	while (true) {
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) break;
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		renderer.RenderColor(0.1f, 0.2f, 0.4f);
	}

	return 0;
}