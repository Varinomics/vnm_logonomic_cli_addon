cmake_minimum_required(VERSION 3.25)

foreach(required SDK_SOURCE INDEX_TOOL VALIDATE_TOOL TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Linux recipe regression requires ${required}.")
    endif()
endforeach()
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
foreach(directory assets capabilities provenance notices)
    file(MAKE_DIRECTORY "${TEST_ROOT}/source/package/${directory}")
endforeach()
file(WRITE "${TEST_ROOT}/source/probe.cpp" [=[
#include <iostream>
int main(int argc, char**) { std::cout << argc << '\n'; return 0; }
]=])
file(WRITE "${TEST_ROOT}/source/manifest.json" [=[
{"bootstrap_version":1,"package_id":"org.example.recipe-probe","release_version":"1.0.0","platforms":["linux-x86_64"],"adapter":{"path":{"linux-x86_64":"adapter/probe"},"argv":[],"environment":{}},"core_protocol_generations":[1],"integrity_index":"package.files.json"}
]=])
set(project [=[
cmake_minimum_required(VERSION 3.25)
project(ordinary_linux_recipe LANGUAGES CXX)
include("@SDK_SOURCE@/cmake/package.cmake")
add_executable(logonomic_cli_addon_index IMPORTED GLOBAL)
set_target_properties(logonomic_cli_addon_index PROPERTIES IMPORTED_LOCATION "@INDEX_TOOL@")
add_executable(probe probe.cpp)
configure_file(manifest.json generated/logonomic-cli-addon.json COPYONLY)
logonomic_cli_addon_add_package(probe_package org.example.recipe-probe package_directory ADAPTER probe)
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${TEST_ROOT}/source/CMakeLists.txt" "${project}")
foreach(mode standalone embedded)
    set(build "${TEST_ROOT}/${mode}")
    set(arguments -S "${TEST_ROOT}/source" -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release)
    if(mode STREQUAL "embedded")
        list(APPEND arguments -DLOGONOMIC_CLI_ADDON_EMBEDDED=ON)
    endif()
    find_program(cache ccache REQUIRED)
    list(APPEND arguments "-DCMAKE_CXX_COMPILER_LAUNCHER=${cache}")
    execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 1 --target probe_package
        COMMAND_ERROR_IS_FATAL ANY)
    set(package "${build}/package/org.example.recipe-probe")
    execute_process(COMMAND "${VALIDATE_TOOL}" "${package}" --platform linux-x86_64
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${package}/adapter/probe"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0 OR NOT output STREQUAL "1\n")
        message(FATAL_ERROR "Plain Release ${mode} executable did not load: ${result}: ${output}: ${error}")
    endif()
endforeach()
