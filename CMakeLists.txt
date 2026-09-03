cmake_minimum_required(VERSION 4.3)
project(VibeZ)

set(CMAKE_CXX_STANDARD 20)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(VibeZ main.cpp
        core/capture/Monitor.hpp
        core/capture/Monitor.cpp
        core/capture/Frame.hpp
        core/capture/Frame.cpp)

target_link_libraries(VibeZ PRIVATE
        fmt::fmt
        spdlog::spdlog
        dxgi.lib
        d3d11.lib
)
