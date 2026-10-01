cmake_minimum_required(VERSION 3.20)
project(EldenChaos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: friends don't need the VC++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(EldenChaos SHARED
    src/dllmain.cpp
    src/udp_server.cpp
    src/command_router.cpp
    src/net_sync.cpp
    src/game/input_fx.cpp
    src/game/memory.cpp
    src/game/params.cpp
    src/game/rtti_dump.cpp
    src/game/player.cpp
    src/game/spawner.cpp
    src/game/targets.cpp
    src/util/config.cpp
    src/util/log.cpp
)

target_include_directories(EldenChaos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(EldenChaos PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(EldenChaos PRIVATE ws2_32)

# Copy signatures.json next to the built DLL
add_custom_command(TARGET EldenChaos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:EldenChaos>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config/signatures.json"
        "$<TARGET_FILE_DIR:EldenChaos>/config/signatures.json"
)
