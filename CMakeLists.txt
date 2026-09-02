cmake_minimum_required(VERSION 4.3)
project(VibeZ)

set(CMAKE_CXX_STANDARD 20)

find_package(fmt CONFIG REQUIRED)

add_executable(VibeZ main.cpp)

target_link_libraries(VibeZ PRIVATE fmt::fmt)
