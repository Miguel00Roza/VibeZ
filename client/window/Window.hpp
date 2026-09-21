#pragma once
#include <windows.h>


class Window {
public:
    HINSTANCE hInstance;
    int nCmdShow;
    HWND hwnd;

    Window(HINSTANCE hInstance, int nCmdShow);

    void Show();
    void Update();
    HWND getHwnd();
};