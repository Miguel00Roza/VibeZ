#include <dxgi.h>
#include <iostream>
#include <windows.h>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <future>
#include <conio.h>

#include "client/capture/Monitor.hpp"
#include "client/capture/CaptureSession.hpp"

using Microsoft::WRL::ComPtr;

void StateSharing(std::atomic<bool> *sharing) {
    while (true) {
        if (_kbhit()) {
            char key = _getch();
            if (key == 's') {
                sharing->store(!(sharing->load()));
                break;
            }
        }
    }
}

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

    std::atomic<bool> sharing(true);

    CaptureSession captureSession{};


    std::future<HRESULT> hr = std::async(&CaptureSession::CaptureScreen, &captureSession, hmonitors[input], 30.0, &sharing);
    std::thread t2(StateSharing, &sharing);

    if (t2.joinable()) {
        t2.join();
    }

    std::cout << hr.get() << std::endl;

    return 0;
}
