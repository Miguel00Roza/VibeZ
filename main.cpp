#include <windows.h>

#include "client/window/Window.hpp"

int WINAPI main(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {

	Window window(hInstance, nCmdShow);

	window.Show();
	window.Update();

	MSG msg = {};
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}