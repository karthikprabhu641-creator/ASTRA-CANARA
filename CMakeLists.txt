cmake_minimum_required(VERSION 3.15)
project(kvdb VERSION 1.0.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -pedantic -Wconversion)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# Core library
add_library(kvdb_core STATIC
    src/storage/format.cpp
    src/storage/file.cpp
    src/storage/index.cpp
    src/recovery/recovery.cpp
    src/concurrency/compactor.cpp
    src/cli/batch_parser.cpp
    src/database.cpp
)

target_include_directories(kvdb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# CLI executable
add_executable(kvdb
    src/cli/main.cpp
)
target_link_libraries(kvdb PRIVATE kvdb_core)

# Torture testing tool
add_executable(torture_tool
    tests/torture/torture_main.cpp
    tests/torture/oracle.cpp
)
target_link_libraries(torture_tool PRIVATE kvdb_core)

# Additive Phase 5 runner; the existing Phase 4 torture_tool remains unchanged.
add_executable(torture_tool_phase5
    tests/torture/torture_phase5_main.cpp
    tests/torture/oracle.cpp
)
target_link_libraries(torture_tool_phase5 PRIVATE kvdb_core Threads::Threads)

add_executable(workload_generator_tests
    tests/torture/workload_generator_test.cpp
)
target_link_libraries(workload_generator_tests PRIVATE kvdb_core)

add_executable(oracle_tests
    tests/torture/oracle_test.cpp
    tests/torture/oracle.cpp
)
target_link_libraries(oracle_tests PRIVATE kvdb_core Threads::Threads)

# Test-only CLI with deterministic transaction/ACK crash hooks.
add_executable(kvdb_crash_test
    src/cli/main.cpp
    src/database.cpp
    src/storage/format.cpp
    src/storage/file.cpp
    src/storage/index.cpp
    src/recovery/recovery.cpp
    src/concurrency/compactor.cpp
    src/cli/batch_parser.cpp
)
target_include_directories(kvdb_crash_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/torture
)
target_compile_definitions(kvdb_crash_test PRIVATE KVDB_ENABLE_CRASH_HOOKS)

# Phase 8 deterministic crash-point verification executable.
add_executable(torture_tool_phase8
    tests/torture/torture_phase8_main.cpp
    tests/torture/child_process.cpp
    tests/torture/oracle.cpp
)
target_include_directories(torture_tool_phase8 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/torture
)
target_link_libraries(torture_tool_phase8 PRIVATE Threads::Threads)

# Isolated Phase 6 child-process crash/recovery runner.
add_executable(torture_tool_phase6
    tests/torture/torture_phase6_main.cpp
    tests/torture/child_process.cpp
    tests/torture/oracle.cpp
)
target_link_libraries(torture_tool_phase6 PRIVATE kvdb_core)

# Basic unit test
add_executable(unit_tests
    tests/unit/basic_test.cpp
    tests/torture/oracle.cpp
)
target_link_libraries(unit_tests PRIVATE kvdb_core)
