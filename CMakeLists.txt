cmake_minimum_required(VERSION 3.20)
project(JARVIS VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(spdlog REQUIRED)

add_library(
    jarvis_proto
    generated/cpp/jarvis.pb.cc
    generated/cpp/jarvis.grpc.pb.cc
)

target_include_directories(
    jarvis_proto
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp
)

target_link_libraries(
    jarvis_proto
    PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

add_executable(
    jarvis
    core/main.cpp
    core/command_handler.cpp
    core/engine.cpp
)

target_link_libraries(
    jarvis
    PRIVATE
    jarvis_proto
)

add_library(
    jarvis_ai_proto
    generated/cpp/ai.pb.cc
    generated/cpp/ai.grpc.pb.cc
)

target_include_directories(
    jarvis_ai_proto
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/cpp
)

target_link_libraries(
    jarvis_ai_proto
    PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

add_executable(
    jarvis_grpc_server
    core/grpc_server_main.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/ai_client.cpp
    core/jarvis_service.cpp
)

target_link_libraries(
    jarvis_grpc_server
    PRIVATE
    jarvis_proto
    jarvis_ai_proto
    spdlog::spdlog
)