
cmake_minimum_required(VERSION 3.15)

project(DRXexpandTM LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MinHook: compile as static library
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

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

add_custom_command(TARGET DRXexpandTM POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    "$<TARGET_FILE_DIR:DRXexpandTM>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:DRXexpandTM>/assets"
)

target_link_libraries(DRXexpandTM PRIVATE winmm)
