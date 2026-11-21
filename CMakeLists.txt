cmake_minimum_required(VERSION 3.21)
include(sdk/logonomic_cli_addon_sdk/cmake/standalone_toolchain.cmake NO_POLICY_SCOPE)
project(vnm_logonomic_cli_addon LANGUAGES CXX)

option(LOGONOMIC_CLI_ADDON_BUILD_HOST "Build the native add-on host and its process helpers" OFF)
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
endif()

add_subdirectory(sdk/logonomic_cli_addon_sdk)
if(LOGONOMIC_CLI_ADDON_BUILD_HOST)
    add_subdirectory(host)
endif()
