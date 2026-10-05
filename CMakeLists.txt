cmake_minimum_required(VERSION 3.27)

project(hypr3d18813 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(HYPRLAND_HEADERS "" CACHE PATH
    "Hyprland headers root containing include/hyprland"
)

if(NOT HYPRLAND_HEADERS)
    message(FATAL_ERROR
        "Pass -DHYPRLAND_HEADERS=/path/to/headersRoot"
    )
endif()

set(HYPRLAND_INCLUDE
    "${HYPRLAND_HEADERS}/include"
)

set(HYPRLAND_ROOT
    "${HYPRLAND_INCLUDE}/hyprland"
)

set(HYPRLAND_SRC
    "${HYPRLAND_ROOT}/src"
)

set(HYPRLAND_PROTOCOLS_INCLUDE
    "${HYPRLAND_ROOT}/protocols"
)

if(NOT EXISTS
    "${HYPRLAND_INCLUDE}/hyprland/src/plugins/PluginAPI.hpp"
)
    message(FATAL_ERROR
        "PluginAPI.hpp not found:\n"
        "${HYPRLAND_INCLUDE}/hyprland/src/plugins/PluginAPI.hpp"
    )
endif()

if(NOT EXISTS
    "${HYPRLAND_PROTOCOLS_INCLUDE}/color-management-v1.hpp"
)
    message(FATAL_ERROR
        "color-management-v1.hpp not found:\n"
        "${HYPRLAND_PROTOCOLS_INCLUDE}/color-management-v1.hpp"
    )
endif()

if(NOT EXISTS
    "${HYPRLAND_SRC}/render/SyncFDManager.hpp"
)
    message(FATAL_ERROR
        "SyncFDManager.hpp not found:\n"
        "${HYPRLAND_SRC}/render/SyncFDManager.hpp"
    )
endif()

find_package(PkgConfig REQUIRED)

# --- Jolt Physics: rigid body simulation for scene objects ------------------
# Included directly via Jolt/Jolt.cmake (the source list + the Jolt target),
# bypassing Jolt's top-level app build. Directory-scope link_libraries keeps
# this working no matter what the plugin target is called.
set(PHYSICS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Jolt")
set(USE_ASSERTS OFF)
set(INTERPROCEDURAL_OPTIMIZATION OFF) # keep plugin builds fast
set(CROSS_PLATFORM_DETERMINISTIC OFF)
include("${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake")
# Hide Jolt's symbols inside the plugin .so: without this the archive
# exports thousands of JPH:: symbols, and the plugin load collides with the
# global symbol namespace ("multiply defined" on load).
target_compile_options(Jolt PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden)
target_include_directories(Jolt PUBLIC "${PHYSICS_REPO_ROOT}")
link_libraries(Jolt)

# Jolt.cmake forces C++17 in directory scope; ours is C++23.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(HYPRGRAPHICS REQUIRED IMPORTED_TARGET hyprgraphics)

add_library(hypr3d18813 SHARED
    src/main.cpp
    src/Render/GLScene.cpp
    src/Render/MapModel.cpp
    src/Render/PlayerModel.cpp
    src/World/MapCollision.cpp
    src/World/Camera.cpp
    src/World/Outline.cpp
    src/World/Picking.cpp
    src/World/ScreenProject.cpp
    src/World/World3D.cpp
    src/Input/InputController.cpp
    src/HyprlandCompat/WindowsCompat.cpp
    src/HyprlandCompat/FocusCompat.cpp
    src/HyprlandCompat/WindowCapture.cpp
    src/HyprlandCompat/PointerHook.cpp
)

target_include_directories(hypr3d18813
    BEFORE PRIVATE
        "${HYPRLAND_INCLUDE}"
        "${HYPRLAND_ROOT}"
        "${HYPRLAND_SRC}"
        "${HYPRLAND_PROTOCOLS_INCLUDE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(hypr3d18813 PRIVATE
    PkgConfig::HYPRGRAPHICS
    PkgConfig::PIXMAN
    PkgConfig::LIBDRM
    PkgConfig::XKBCOMMON
    GLESv2
    EGL
    dl
)

set_target_properties(hypr3d18813 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hypr3d18813"
)
