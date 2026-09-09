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
    core/plugin_loader.cpp
    core/dynamic_library.cpp
)

target_include_directories(
    jarvis
    PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk
)

target_link_libraries(
    jarvis
    PRIVATE
    jarvis_proto
    spdlog::spdlog
    ${CMAKE_DL_LIBS}
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
    core/plugin_loader.cpp
    core/dynamic_library.cpp
)

target_include_directories(
    jarvis_grpc_server
    PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk
)

target_link_libraries(
    jarvis_grpc_server
    PRIVATE
    jarvis_proto
    jarvis_ai_proto
    spdlog::spdlog
    ${CMAKE_DL_LIBS}
)

add_executable(
    jarvis_tests
    tests/capability_registry_test.cpp
    tests/plugin_config_test.cpp
    tests/consent_gate_test.cpp
    tests/minimal_json_test.cpp
    tests/plugin_loader_test.cpp
    tests/system_control_plugin_test.cpp
    plugins/system-control/plugin_internal.cpp
    core/capability_registry.cpp
    core/command_handler.cpp
    core/engine.cpp
    core/plugin_config.cpp
    core/consent_gate.cpp
    core/minimal_json.cpp
    core/plugin_loader.cpp
    core/dynamic_library.cpp
)

target_include_directories(
    jarvis_tests
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/plugin_sdk
)

target_link_libraries(
    jarvis_tests
    PRIVATE
    GTest::gtest_main
    spdlog::spdlog
    ${CMAKE_DL_LIBS}
)

add_library(valid_echo_fixture SHARED tests/fixtures/plugins/valid-echo/plugin.cpp)
target_include_directories(valid_echo_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(valid_echo_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/valid-echo
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/valid-echo
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET valid_echo_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/valid-echo/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/valid-echo/manifest.json)

add_library(bad_abi_fixture SHARED tests/fixtures/plugins/bad-abi/plugin.cpp)
target_include_directories(bad_abi_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(bad_abi_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/bad-abi
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/bad-abi
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET bad_abi_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/bad-abi/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/bad-abi/manifest.json)

add_library(mismatched_tier_fixture SHARED tests/fixtures/plugins/mismatched-tier/plugin.cpp)
target_include_directories(mismatched_tier_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(mismatched_tier_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/mismatched-tier
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/mismatched-tier
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET mismatched_tier_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/mismatched-tier/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/mismatched-tier/manifest.json)

add_library(blocking_fixture SHARED tests/fixtures/plugins/blocking/plugin.cpp)
target_include_directories(blocking_fixture PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(blocking_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/blocking
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/blocking
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET blocking_fixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/blocking/manifest.json
        ${CMAKE_BINARY_DIR}/tests/fixtures/plugins/blocking/manifest.json)

add_library(system_info_plugin SHARED plugins/system-info/plugin.cpp)
target_include_directories(system_info_plugin PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(system_info_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-info
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-info
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET system_info_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/plugins/system-info/manifest.json
        ${CMAKE_BINARY_DIR}/plugins/system-info/manifest.json)

add_dependencies(jarvis system_info_plugin)
add_dependencies(jarvis_grpc_server system_info_plugin)
add_dependencies(jarvis_tests system_info_plugin)

add_library(system_control_plugin SHARED
    plugins/system-control/plugin.cpp
    plugins/system-control/plugin_internal.cpp)
target_include_directories(system_control_plugin PRIVATE ${CMAKE_SOURCE_DIR}/plugin_sdk)
set_target_properties(system_control_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-control
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/system-control
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
add_custom_command(TARGET system_control_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/plugins/system-control/manifest.json
        ${CMAKE_BINARY_DIR}/plugins/system-control/manifest.json)

add_dependencies(jarvis system_control_plugin)
add_dependencies(jarvis_grpc_server system_control_plugin)
add_dependencies(jarvis_tests system_control_plugin)

add_dependencies(jarvis_tests valid_echo_fixture bad_abi_fixture mismatched_tier_fixture blocking_fixture)

target_compile_definitions(jarvis_tests PRIVATE
    JARVIS_TEST_FIXTURES_DIR="${CMAKE_BINARY_DIR}/tests/fixtures/plugins"
    JARVIS_BUILD_PLUGINS_DIR="${CMAKE_BINARY_DIR}/plugins")

include(GoogleTest)
gtest_discover_tests(jarvis_tests)