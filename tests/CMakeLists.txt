# Standalone, hardware-free unit tests. Needs only a C++23 compiler and the glm submodule:
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
# No Vulkan, GLFW, JNI headers or GPU required.
cmake_minimum_required(VERSION 3.15)
project(mcvr-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MCVR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
if(NOT EXISTS ${MCVR_ROOT}/extern/glm/glm/glm.hpp)
    message(FATAL_ERROR "glm submodule missing: run 'git submodule update --init extern/glm'")
endif()

find_package(Threads REQUIRED)

add_executable(mcvr_tests
    main.cpp
    test_singleton.cpp
    test_parallel.cpp
    test_shared_layout.cpp)
target_include_directories(mcvr_tests PRIVATE ${MCVR_ROOT}/src ${MCVR_ROOT}/extern/glm)
target_link_libraries(mcvr_tests PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(mcvr_tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME mcvr_unit COMMAND mcvr_tests)
set_tests_properties(mcvr_unit PROPERTIES TIMEOUT 60)
