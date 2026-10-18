cmake_minimum_required(VERSION 3.15)
project(mcvr_singleton_diagnostics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_executable(mcvr_singleton_tests singleton_tests.cpp)
target_include_directories(mcvr_singleton_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(mcvr_singleton_tests PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(mcvr_singleton_tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME singleton_full COMMAND mcvr_singleton_tests)
foreach(case_name IN ITEMS
    singleton_uninitialized
    singleton_init_argument
    singleton_double_init
    singleton_failed_construction_retry
    singleton_concurrent_init)
    add_test(NAME "isolate_${case_name}" COMMAND mcvr_singleton_tests --run-exact "${case_name}")
endforeach()
add_test(NAME singleton_missing_selector_rejected COMMAND mcvr_singleton_tests --run-exact missing_case)
set_tests_properties(singleton_missing_selector_rejected PROPERTIES WILL_FAIL TRUE)
set_tests_properties(singleton_full PROPERTIES TIMEOUT 60)
