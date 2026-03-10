cmake_minimum_required(VERSION 3.20)
project(JARVIS VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(
    jarvis
    core/main.cpp
    core/command_handler.cpp
    core/engine.cpp
)