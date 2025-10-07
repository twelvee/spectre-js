cmake_minimum_required(VERSION 3.23)
project(spectre_js VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(spectre STATIC
    src/config.cpp
    src/context.cpp
    src/runtime.cpp
    src/mode_factory.cpp
    src/mode_single_thread.cpp
    src/mode_multi_thread.cpp
    src/mode_helpers.cpp
    src/subsystems.cpp
    es2025/environment.cpp
    es2025/modules/array_buffer_module.cpp
    es2025/modules/array_module.cpp
    es2025/modules/async_function_module.cpp
    es2025/modules/async_iterator_module.cpp
    es2025/modules/atomics_module.cpp
    es2025/modules/bigint_module.cpp
    es2025/modules/boolean_module.cpp
    es2025/modules/data_view_module.cpp
    es2025/modules/date_module.cpp
    es2025/modules/error_module.cpp
    es2025/modules/finalization_registry_module.cpp
    es2025/modules/function_module.cpp
    es2025/modules/generator_module.cpp
    es2025/modules/global_module.cpp
    es2025/modules/intl_module.cpp
    es2025/modules/iterator_module.cpp
    es2025/modules/json_module.cpp
    es2025/modules/map_module.cpp
    es2025/modules/math_module.cpp
    es2025/modules/module_loader_module.cpp
    es2025/modules/number_module.cpp
    es2025/modules/object_module.cpp
    es2025/modules/promise_module.cpp
    es2025/modules/proxy_module.cpp
    es2025/modules/reflect_module.cpp
    es2025/modules/regexp_module.cpp
    es2025/modules/set_module.cpp
    es2025/modules/shadow_realm_module.cpp
    es2025/modules/shared_array_buffer_module.cpp
    es2025/modules/string_module.cpp
    es2025/modules/structured_clone_module.cpp
    es2025/modules/symbol_module.cpp
    es2025/modules/temporal_module.cpp
    es2025/modules/typed_array_module.cpp
    es2025/modules/weak_map_module.cpp
    es2025/modules/weak_ref_module.cpp
    es2025/modules/weak_set_module.cpp
)

target_include_directories(spectre PUBLIC include)

target_compile_definitions(spectre PRIVATE $<$<CONFIG:Debug>:SPECTRE_DEBUG_BUILD>)

add_executable(spectre_example_basic examples/basic_demo.cpp)
add_executable(spectre_example_multi examples/multi_thread_demo.cpp)
add_executable(spectre_example_showcase examples/es_showcase.cpp)
target_link_libraries(spectre_example_basic PRIVATE spectre)
target_link_libraries(spectre_example_multi PRIVATE spectre)
target_link_libraries(spectre_example_showcase PRIVATE spectre)

set_target_properties(spectre_example_basic PROPERTIES OUTPUT_NAME "spectre_example_basic")
set_target_properties(spectre_example_multi PROPERTIES OUTPUT_NAME "spectre_example_multi")
set_target_properties(spectre_example_showcase PROPERTIES OUTPUT_NAME "spectre_example_showcase")

option(SPECTRE_BUILD_TESTS "Build spectre tests" ON)

if(SPECTRE_BUILD_TESTS)
    enable_testing()
    add_executable(spectre_tests tests/runtime_tests.cpp)
    target_link_libraries(spectre_tests PRIVATE spectre)
    add_test(NAME spectre_runtime COMMAND spectre_tests)
endif()

add_executable(spectre_example_es2025 examples/es2025_production_demo.cpp)
target_link_libraries(spectre_example_es2025 PRIVATE spectre)
