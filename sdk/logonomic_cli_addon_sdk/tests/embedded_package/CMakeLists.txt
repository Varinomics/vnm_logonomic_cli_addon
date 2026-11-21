cmake_minimum_required(VERSION 3.21)
project(logonomic_cli_addon_embedded_fixture LANGUAGES CXX)

# A package project that builds both on its own and embedded, as
# docs/package_contract.md ("Embedded build") requires of every package. It
# assembles the SDK's conformant fixture package from the fixture adapter and
# the fixture files; tests/embedded_package_tests.cmake builds it both ways.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only a standalone build acquires the SDK; an embedding build owns it.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must add the SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
        "Logonomic CLI add-on SDK source")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" sdk)
endif()

set(fixture_package_id org.example.unknown-fixture)
set(fixture_files "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/packages")
file(GLOB_RECURSE fixture_inputs CONFIGURE_DEPENDS
    "${fixture_files}/_shared/*"
    "${fixture_files}/${fixture_package_id}/*")
logonomic_cli_addon_package_directory(${fixture_package_id} fixture_package_directory)
add_custom_command(
    OUTPUT "${fixture_package_directory}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${fixture_package_directory}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${fixture_files}/_shared" "${fixture_package_directory}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${fixture_files}/${fixture_package_id}" "${fixture_package_directory}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${fixture_package_directory}/adapter"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "$<TARGET_FILE:logonomic_cli_addon_unknown_fixture_adapter>"
        "${fixture_package_directory}/adapter/"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${fixture_package_directory}"
        --third-party assets/mark.svg notices/mark-license.txt provenance/mark.json
    DEPENDS
        logonomic_cli_addon_index
        logonomic_cli_addon_unknown_fixture_adapter
        ${fixture_inputs}
    COMMENT "Assembling package ${fixture_package_id}"
    VERBATIM)
add_custom_target(logonomic_cli_addon_embedded_fixture_package ALL
    DEPENDS "${fixture_package_directory}/package.files.json")
logonomic_cli_addon_install_package("${fixture_package_directory}" ${fixture_package_id}
    TARGET logonomic_cli_addon_embedded_fixture_package)
