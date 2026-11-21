cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
project(logonomic_cli_addon_filesystem_observer_tests LANGUAGES C CXX)

# This optional component uses Qt; the ordinary SDK test project does not.
if(NOT EXISTS "${VNM_FILESYSTEM_OBSERVATION_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "VNM_FILESYSTEM_OBSERVATION_SOURCE_DIR must name the selected filesystem observation source")
endif()
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF)
set(VNM_FILESYSTEM_OBSERVATION_BUILD_TESTS OFF)
set(VNM_FILESYSTEM_OBSERVATION_ENABLE_TEST_HOOKS OFF)
add_subdirectory("${VNM_FILESYSTEM_OBSERVATION_SOURCE_DIR}" observation)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." sdk)
logonomic_cli_addon_add_filesystem_observer()

enable_testing()
add_executable(logonomic_cli_addon_filesystem_observer_tests
    filesystem_lifecycle_observer_tests.cpp)
target_link_libraries(logonomic_cli_addon_filesystem_observer_tests PRIVATE
    logonomic_cli_addon_sdk::filesystem_observer)
logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_filesystem_observer_tests)
add_test(NAME logonomic_cli_addon_filesystem_observer_tests
    COMMAND logonomic_cli_addon_filesystem_observer_tests)
set_tests_properties(logonomic_cli_addon_filesystem_observer_tests PROPERTIES TIMEOUT 10)
