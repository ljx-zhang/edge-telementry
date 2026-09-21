cmake_minimum_required(VERSION 3.20)
project(edge_telemetry VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(SQLITE3_ROOT "" CACHE PATH "SQLite installation root")

find_path(SQLITE3_INCLUDE_DIR sqlite3.h
    HINTS "${SQLITE3_ROOT}"
    PATH_SUFFIXES include Library/include)
find_library(SQLITE3_LIBRARY NAMES sqlite3
    HINTS "${SQLITE3_ROOT}"
    PATH_SUFFIXES lib Library/lib)

if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite3 not found. Configure with -DSQLITE3_ROOT=<installation root>.")
endif()

add_library(edge_core
    src/net.cpp
    src/store.cpp)
target_include_directories(edge_core PUBLIC src "${SQLITE3_INCLUDE_DIR}")
target_link_libraries(edge_core PUBLIC "${SQLITE3_LIBRARY}")

if(WIN32)
    target_link_libraries(edge_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(edge_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(edge_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(edge_agent src/edge_agent.cpp)
target_link_libraries(edge_agent PRIVATE edge_core)

add_executable(telemetry_receiver src/telemetry_receiver.cpp)
target_link_libraries(telemetry_receiver PRIVATE edge_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE edge_core)
    add_test(NAME protocol_test COMMAND protocol_test)
endif()
if(BUILD_TESTING AND WIN32)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell)
    if(POWERSHELL_EXECUTABLE)
        add_test(
            NAME integration_demo
            COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/integration_demo.ps1"
                -BuildDir "$<TARGET_FILE_DIR:edge_agent>" -Port 19310
                -AgentRuntimeSeconds 2 -IntervalMilliseconds 80)
        add_test(
            NAME fault_injection_demo
            COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fault_injection_demo.ps1"
                -BuildDir "$<TARGET_FILE_DIR:edge_agent>" -BasePort 19320)
        set_tests_properties(integration_demo fault_injection_demo PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

if(WIN32)
    find_file(SQLITE3_RUNTIME_DLL sqlite3.dll
        HINTS "${SQLITE3_ROOT}"
        PATH_SUFFIXES bin Library/bin)
    if(SQLITE3_RUNTIME_DLL)
        foreach(target edge_agent telemetry_receiver)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SQLITE3_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>")
        endforeach()
    endif()
endif()
