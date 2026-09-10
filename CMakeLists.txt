cmake_minimum_required(VERSION 4.3)
project(VibeZ)

set(CMAKE_CXX_STANDARD 20)

find_package(spdlog CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_executable(VibeZ main.cpp
        client/capture/Monitor.hpp
        client/capture/Monitor.cpp
        client/capture/Frame.hpp
        client/capture/Frame.cpp
        client/capture/CaptureSession.cpp
        client/capture/CaptureSession.hpp
        client/capture/TexturePool.cpp
        client/capture/TexturePool.hpp)

target_include_directories(VibeZ PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(VibeZ PRIVATE
        spdlog::spdlog
        dxgi.lib
        d3d11.lib
)
