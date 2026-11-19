cmake_minimum_required(VERSION 3.20)
project(mcvr_layer3a_smoke LANGUAGES CXX)

# Standalone project: the Layer-3A smoke must not drag in the renderer's
# recursive dependency tree (JNI, Minecraft, DLSS/NRD/FFX shader SDKs). It only
# needs a C++23 compiler, the Vulkan loader/headers and GLFW.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)

set(MCVR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_library(mcvr_layer3a_common INTERFACE)
target_include_directories(mcvr_layer3a_common INTERFACE "${MCVR_REPO_ROOT}/src")
target_link_libraries(mcvr_layer3a_common INTERFACE Vulkan::Vulkan PkgConfig::GLFW3)
target_compile_options(mcvr_layer3a_common INTERFACE -Wall -Wextra -Wpedantic)

# Test-only target: may use Admission::TestAllowCpuDevice.
add_executable(layer3a_smoke smoke.cpp)
target_link_libraries(layer3a_smoke PRIVATE mcvr_layer3a_common)
target_compile_definitions(layer3a_smoke PRIVATE MCVR_LAYER3A_TEST_CPU_ADMISSION=1)

# Production-policy target: same source, no CPU admission compiled in.
add_executable(layer3a_production_policy smoke.cpp)
target_link_libraries(layer3a_production_policy PRIVATE mcvr_layer3a_common)
