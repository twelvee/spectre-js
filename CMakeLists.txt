cmake_minimum_required(VERSION 3.23)
project(spectre_js VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(spectre STATIC
    src/config.cpp
    src/context.cpp
    src/runtime.cpp
    src/mode_single_thread.cpp
    src/subsystems.cpp
)

target_include_directories(spectre PUBLIC include)

target_compile_definitions(spectre PRIVATE $<$<CONFIG:Debug>:SPECTRE_DEBUG_BUILD>)

add_executable(spectre_example_basic examples/basic_demo.cpp)
target_link_libraries(spectre_example_basic PRIVATE spectre)

set_target_properties(spectre_example_basic PROPERTIES OUTPUT_NAME "spectre_example_basic")

option(SPECTRE_BUILD_TESTS "Build spectre tests" ON)

if(SPECTRE_BUILD_TESTS)
    enable_testing()
    add_executable(spectre_tests tests/runtime_tests.cpp)
    target_link_libraries(spectre_tests PRIVATE spectre)
    add_test(NAME spectre_runtime COMMAND spectre_tests)
endif()


