cmake_minimum_required(VERSION 3.20)
project(JARVIS VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(GTest REQUIRED)

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
    core/capability_registry.cpp
    core/plugin_config.cpp
    core/consent_gate.cpp
    core/minimal_json.cpp
)

target_link_libraries(
    jarvis
    PRIVATE
    jarvis_proto
    spdlog::spdlog
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
    core/capability_registry.cpp
    core/plugin_config.cpp
    core/consent_gate.cpp
    core/ai_client.cpp
    core/jarvis_service.cpp
    core/minimal_json.cpp
)

target_link_libraries(
    jarvis_grpc_server
    PRIVATE
    jarvis_proto
    jarvis_ai_proto
    spdlog::spdlog
)

add_executable(
    jarvis_tests
    tests/capability_registry_test.cpp
    tests/plugin_config_test.cpp
    tests/consent_gate_test.cpp
    tests/minimal_json_test.cpp
    core/capability_registry.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/plugin_config.cpp
    core/consent_gate.cpp
    core/minimal_json.cpp
)

target_include_directories(
    jarvis_tests
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

target_link_libraries(
    jarvis_tests
    PRIVATE
    GTest::gtest_main
    spdlog::spdlog
)

include(GoogleTest)
gtest_discover_tests(jarvis_tests)