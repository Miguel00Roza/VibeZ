#pragma once
#include <string>
#include <vector>
#include <Windows.h>

struct Resolution {
    int width;
    int height;
};

class Monitor {
private:
    HMONITOR hMonitor;
    std::string szDevice;
    Resolution resolution{};

public:
    Monitor(const HMONITOR &hMonitor, const std::string &szDevice, const Resolution &resolution) {
        this->hMonitor = hMonitor;
        this->szDevice = szDevice;
        this->resolution = resolution;
    }

    [[nodiscard]] HMONITOR get_h_monitor() const {
        return hMonitor;
    }

    [[nodiscard]] std::string get_sz_device() const {
        return szDevice;
    }

    [[nodiscard]] Resolution get_resolution() const {
        return resolution;
    }
};

std::vector<Monitor> get_monitors();
