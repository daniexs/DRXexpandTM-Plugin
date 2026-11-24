
cmake_minimum_required(VERSION 3.15)

project(DRXexpandTM LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook: compile as static library
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vignette.hlsl" DRX_VIGNETTE_RESOURCE_PATH)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/fly.mp3" DRX_FLY_RESOURCE_PATH)
configure_file(
    src/plugin_resources.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/plugin_resources.rc"
    @ONLY
)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(minhook)

add_library(DRXexpandTM SHARED
    src/plugin.cpp
    src/dx11_hook.cpp
    src/renderer.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/plugin_resources.rc"
    DRXexpandTM.def
)

target_include_directories(DRXexpandTM PRIVATE
    sdk/include
    external/minhook/include
)

target_link_libraries(DRXexpandTM PRIVATE
    minhook
    d3d11
    dxgi
    d3dcompiler
)

set_target_properties(DRXexpandTM PROPERTIES
    PREFIX ""
)

target_link_libraries(DRXexpandTM PRIVATE winmm)
