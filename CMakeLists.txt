cmake_minimum_required(VERSION 3.21)

project(phi-adapter-webos VERSION 0.1.0 DESCRIPTION "Phi LG webOS TV adapter" LANGUAGES CXX)

include(GNUInstallDirs)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json 3.11 REQUIRED)
# The TLS socket under the TV's WebSocket is this adapter's own: the SDK keeps
# its OpenSSL private.
find_package(OpenSSL REQUIRED)

set(PHI_ADAPTER_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phi-adapter-sdk" CACHE PATH
    "Path to local phi-adapter-sdk checkout")
option(PHI_ADAPTER_WEBOS_USE_LOCAL_ADAPTER_SDK "Use local ../phi-adapter-sdk" ON)

if(NOT TARGET phi::adapter-sdk)
    if(PHI_ADAPTER_WEBOS_USE_LOCAL_ADAPTER_SDK AND EXISTS "${PHI_ADAPTER_SDK_SOURCE_DIR}/CMakeLists.txt")
        set(PHI_ADAPTER_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${PHI_ADAPTER_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/phi-adapter-sdk")
    else()
        find_package(phi-adapter-sdk CONFIG REQUIRED)
    endif()
endif()

add_library(phi_adapter_webos_core STATIC
    src/webos_instance.cpp
    src/webos_json.cpp
    src/webos_model.cpp
    src/webos_pairing.cpp
    src/webos_protocol.cpp
    src/webos_schema.cpp
    src/webos_settings.cpp
    src/webos_socket.cpp
    src/webos_ws_client.cpp
    src/webos_wol.cpp
)
target_compile_features(phi_adapter_webos_core PUBLIC cxx_std_20)
set_target_properties(phi_adapter_webos_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(phi_adapter_webos_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(phi_adapter_webos_core
    PUBLIC phi::adapter-sdk nlohmann_json::nlohmann_json
    PRIVATE OpenSSL::SSL OpenSSL::Crypto)

add_executable(phi_adapter_webos_ipc src/main.cpp)
target_link_libraries(phi_adapter_webos_ipc PRIVATE phi_adapter_webos_core)
set_target_properties(phi_adapter_webos_ipc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/adapters")

enable_testing()
# SSAP messages and what the answers mean, without a TV.
add_executable(webos_protocol_tests tests/webos_protocol_tests.cpp)
target_link_libraries(webos_protocol_tests PRIVATE phi_adapter_webos_core)
add_test(NAME webos_protocol_tests COMMAND webos_protocol_tests)
set_tests_properties(webos_protocol_tests PROPERTIES TIMEOUT 120)

# The WebSocket client against a loopback server, plain and TLS.
add_executable(webos_ws_client_tests tests/webos_ws_client_tests.cpp)
target_link_libraries(webos_ws_client_tests PRIVATE phi_adapter_webos_core OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME webos_ws_client_tests COMMAND webos_ws_client_tests)
set_tests_properties(webos_ws_client_tests PROPERTIES TIMEOUT 120)

install(TARGETS phi_adapter_webos_ipc
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/webos-config.json"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/adapters)
