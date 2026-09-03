#include "Monitor.hpp"

#include <Windows.h>
#include <vector>

BOOL CALLBACK Monitorenumproc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    const int width = lprcMonitor->right - lprcMonitor->left;
    const int height = lprcMonitor->bottom - lprcMonitor->top;

    MONITORINFOEXA info{};
    info.cbSize = sizeof(info);

    GetMonitorInfoA(hMonitor, &info);

    reinterpret_cast<std::vector<Monitor>*>(dwData)->emplace_back(hMonitor, info.szDevice, Resolution{width, height});

    return true;
}


std::vector<Monitor> get_monitors() {
    std::vector<Monitor> monitors{};

    EnumDisplayMonitors(nullptr, nullptr, Monitorenumproc, reinterpret_cast<LPARAM>(&monitors));

    return monitors;
}