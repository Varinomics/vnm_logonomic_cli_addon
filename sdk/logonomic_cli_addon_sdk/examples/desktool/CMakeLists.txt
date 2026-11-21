cmake_minimum_required(VERSION 3.25)
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(CMAKE_GENERATOR STREQUAL "FASTBuild")
        set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Capture configure environment" FORCE)
        list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
    endif()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()
project(desktool_addon VERSION 1.0.0 LANGUAGES CXX)
set(desktool_package_id org.example.desktool)

if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
        "Directory containing the Logonomic CLI add-on SDK")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" sdk EXCLUDE_FROM_ALL)
endif()

add_executable(desktool_adapter adapter.cpp)
target_compile_definitions(desktool_adapter PRIVATE LOGONOMIC_CLI_ADDON_ADAPTER_VERSION="${PROJECT_VERSION}")
target_link_libraries(desktool_adapter PRIVATE logonomic_cli_addon_sdk::sdk)
if(MINGW)
    # The host runs the adapter from its package with a filtered environment.
    target_link_options(desktool_adapter PRIVATE -static)
endif()

logonomic_cli_addon_package_platform(desktool_platform)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
get_target_property(desktool_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
logonomic_cli_addon_add_package(desktool_package "${desktool_package_id}" desktool_package_directory
    ADAPTER desktool_adapter
    PROJECT_LICENSE "${desktool_sdk_source}/../../LICENSE")

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
add_executable(desktool desktool.cpp)
set_target_properties(desktool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
if(MINGW)
    target_link_options(desktool PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_custom_target(desktool_test_tools ALL DEPENDS
        logonomic_cli_addon_validate logonomic_cli_addon_conformance logonomic_cli_addon_host_simulator)
    add_test(NAME desktool_validation COMMAND logonomic_cli_addon_validate "${desktool_package_directory}")
    add_test(NAME desktool_conformance COMMAND logonomic_cli_addon_conformance "${desktool_package_directory}")
    add_test(NAME desktool_launcher COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher.py" "$<TARGET_FILE:logonomic_cli_addon_host_simulator>"
        "${desktool_package_directory}" "${desktool_platform}" "$<TARGET_FILE:desktool>")
    set_tests_properties(desktool_launcher PROPERTIES TIMEOUT 45)
    add_test(NAME desktool_development_profile COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/development_profile.py")
    add_custom_target(desktool_dev
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/develop.py" prepare
            "${CMAKE_CTEST_COMMAND}" "${CMAKE_CURRENT_BINARY_DIR}" "${desktool_package_directory}"
            "$<TARGET_FILE:desktool>" "--config=$<CONFIG>"
        USES_TERMINAL VERBATIM)
    add_dependencies(desktool_dev desktool_package desktool desktool_test_tools)
endif()
