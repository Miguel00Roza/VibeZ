#include <dxgi.h>
#include <iostream>
#include <Windows.h>
#include <vector>
#include <unordered_map>
#include "client/capture/Monitor.hpp"
#include "client/capture/CaptureSession.hpp"

using Microsoft::WRL::ComPtr;

int main() {

    std::vector<Monitor> monitors;
    monitors = get_monitors();

    std::unordered_map<int, HMONITOR> hmonitors;


    int i = 1;
    std::cout << "Escolha um monitor" << std::endl;
    for (const Monitor &monitor : monitors) {
        std::cout << i << ". ";
        std::cout << monitor.get_h_monitor() << ": " << monitor.get_resolution().width << "x" << monitor.get_resolution().height << std::endl;
        hmonitors.insert({i, monitor.get_h_monitor()});
        ++i;
    }

    int input;
    std::cin >> input;

    CaptureSession captureSession{};
    HRESULT hr = captureSession.CaptureScreen(hmonitors[input], 30.0);
    std::cout << hr << std::endl;

    return 0;
}
