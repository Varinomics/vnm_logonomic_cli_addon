cmake_minimum_required(VERSION 3.21)
include(cmake/standalone_toolchain.cmake NO_POLICY_SCOPE)
set(sdk_version_header "${CMAKE_CURRENT_SOURCE_DIR}/include/logonomic_cli_addon/version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sdk_version_header}")
file(READ "${sdk_version_header}" sdk_version_source)
foreach(part IN ITEMS major minor patch)
    if(NOT sdk_version_source MATCHES "(^|\n)inline constexpr int k_sdk_version_${part} *= *(0|[1-9][0-9]*);[ \t\r]*(\n|$)")
        message(FATAL_ERROR "Expected a decimal SDK ${part} version in ${sdk_version_header}")
    endif()
    set(sdk_version_${part} "${CMAKE_MATCH_2}")
endforeach()
project(logonomic_cli_addon_sdk VERSION "${sdk_version_major}.${sdk_version_minor}.${sdk_version_patch}" LANGUAGES CXX)

# The SDK is a complete top-level project and is also consumed in-tree through
# add_subdirectory. Standalone, it registers its tests itself; in-tree, the
# consuming project registers the same executables so that its own test
# labelling applies.
option(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS "Build the SDK's own test executable" ON)

if(PROJECT_IS_TOP_LEVEL OR CMAKE_PROJECT_NAME STREQUAL "vnm_logonomic_cli_addon")
    set(_logonomic_cli_addon_sdk_top_level ON)
else()
    set(_logonomic_cli_addon_sdk_top_level OFF)
endif()

if(_logonomic_cli_addon_sdk_top_level AND NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

find_package(Threads REQUIRED)

function(logonomic_cli_addon_sdk_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

set(_logonomic_cli_addon_sdk_sources
    src/account_pet.cpp
    src/account_refresh.cpp
    src/accounts.cpp
    src/adapter_runtime.cpp
    src/assistant_reply_evidence.cpp
    src/bootstrap.cpp
    src/conversation_export.cpp
    src/context_state.cpp
    src/execution_context.cpp
    src/model_catalog.cpp
    src/cooperative_description.cpp
    src/model_state.cpp
    src/child_state.cpp
    src/decimal_parse.cpp
    src/frame.cpp
    src/generation_digest.cpp
    src/helper_client.cpp
    src/helper_ingress.cpp
    src/host_simulator.cpp
    src/ids.cpp
    src/integrity_index.cpp
    src/json.cpp
    src/launch_preparation.cpp
    src/launcher_approval.cpp
    src/launcher_approval_editing.cpp
    src/launcher_discovery.cpp
    $<$<PLATFORM_ID:Windows>:src/npm_node_shim.cpp>
    src/launcher_utilities.cpp
    src/lifecycle_events.cpp
    src/lifecycle_correlation_lifetime.cpp
    src/managed_assets.cpp
    src/native_account_profile.cpp
    src/package_source.cpp
    src/package_validator.cpp
    src/permission_snapshot.cpp
    src/presentation_record.cpp
    src/process_proposal.cpp
    src/probe.cpp
    src/probe_observations.cpp
    src/profile_storage.cpp
    src/protocol.cpp
    src/provider_identity.cpp
    src/provider_event_diagnostic.cpp
    src/session_contracts.cpp
    src/session_wake.cpp
    src/terminal_identity.cpp
    src/startup_observer.cpp
    src/session_observation.cpp
    src/managed_configuration.cpp
    src/environment_bootstrap.cpp
    src/sha256.cpp
    src/sidecar.cpp
    src/strict_json.cpp
    src/svg_subset.cpp
    src/transcript.cpp
    src/usage_scan.cpp
    src/usage_incremental.cpp
    src/usage_repository_attribution.cpp
    src/usage_owned_event_correction.cpp
    src/usage_session.cpp
    src/usage_lifetime_summary.cpp
    src/usage_saved_daily_tokens.cpp
    src/usage_cross_source_event.cpp
    src/usage_temporal.cpp
    src/usage_cache.cpp)

if(WIN32)
    list(APPEND _logonomic_cli_addon_sdk_sources
        src/adapter_stdio_win.cpp
        src/package_source_win.cpp
        src/simulator_process_win.cpp)
else()
    list(APPEND _logonomic_cli_addon_sdk_sources
        src/adapter_stdio_posix.cpp
        src/package_source_posix.cpp
        src/simulator_process_posix.cpp)
endif()

add_library(logonomic_cli_addon_sdk_headers INTERFACE)
add_library(
    logonomic_cli_addon_sdk::headers
    ALIAS logonomic_cli_addon_sdk_headers)
target_compile_features(logonomic_cli_addon_sdk_headers INTERFACE cxx_std_20)
target_include_directories(logonomic_cli_addon_sdk_headers
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(logonomic_cli_addon_sdk STATIC ${_logonomic_cli_addon_sdk_sources})
add_library(logonomic_cli_addon_sdk::sdk ALIAS logonomic_cli_addon_sdk)
target_include_directories(logonomic_cli_addon_sdk
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(logonomic_cli_addon_sdk PUBLIC
    logonomic_cli_addon_sdk::headers
    Threads::Threads)
logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_sdk)

# Provider tests opt into these headers; SDK consumers do not inherit them.
add_library(logonomic_cli_addon_sdk_test_support INTERFACE)
add_library(logonomic_cli_addon_sdk::test_support ALIAS logonomic_cli_addon_sdk_test_support)
target_include_directories(logonomic_cli_addon_sdk_test_support
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
target_link_libraries(logonomic_cli_addon_sdk_test_support INTERFACE logonomic_cli_addon_sdk::sdk)

# Tools and the fixture adapter.
foreach(_tool IN ITEMS validate index host_simulator conformance stage)
    add_executable(logonomic_cli_addon_${_tool} tools/logonomic_cli_addon_${_tool}.cpp)
    target_link_libraries(logonomic_cli_addon_${_tool} PRIVATE logonomic_cli_addon_sdk::sdk)
    logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_${_tool})
    if(MINGW)
        target_link_options(logonomic_cli_addon_${_tool} PRIVATE -municode)
    endif()
endforeach()
target_sources(logonomic_cli_addon_validate PRIVATE tools/desktop_inspection.cpp)
target_include_directories(logonomic_cli_addon_stage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(logonomic_cli_addon_unknown_fixture_adapter fixtures/unknown_fixture_adapter.cpp)
set_target_properties(logonomic_cli_addon_unknown_fixture_adapter PROPERTIES
    OUTPUT_NAME unknown-fixture-adapter)
target_link_libraries(logonomic_cli_addon_unknown_fixture_adapter PRIVATE logonomic_cli_addon_sdk::sdk)
logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_unknown_fixture_adapter)

add_executable(logonomic_cli_addon_unknown_fixture_helper fixtures/unknown_fixture_helper.cpp)
set_target_properties(logonomic_cli_addon_unknown_fixture_helper PROPERTIES OUTPUT_NAME unknown_fixture_helper)
target_link_libraries(logonomic_cli_addon_unknown_fixture_helper PRIVATE logonomic_cli_addon_sdk::sdk)
logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_unknown_fixture_helper)

include(fixtures/packages.cmake)
include(cmake/package.cmake)
include(cmake/filesystem_lifecycle_observer.cmake)

function(logonomic_cli_addon_sdk_test_groups output)
    set(groups --exclude-group fixture_paced_progress_lifetime)
    if(APPLE)
        # macOS runs the SDK contracts without the Windows/Linux fixture suites.
        foreach(group IN ITEMS
            observation_fixture_subsets simulator_windows_native_limits
            fixture_positive_package fixture_package_file_variants fixture_behavior_variants fixture_transcripts)
            list(APPEND groups --exclude-group ${group})
        endforeach()
    endif()
    set(${output} ${groups} PARENT_SCOPE)
endfunction()

if(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS)
    add_executable(logonomic_cli_addon_launcher_utilities_tests tests/launcher_utilities_tests.cpp)
    target_link_libraries(logonomic_cli_addon_launcher_utilities_tests PRIVATE logonomic_cli_addon_sdk::sdk)
    logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_launcher_utilities_tests)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(logonomic_cli_addon_launcher_utilities_tests PRIVATE
            LOGONOMIC_TEST_WRAP_PROBE_IO)
        target_link_options(logonomic_cli_addon_launcher_utilities_tests PRIVATE
            "LINKER:--wrap=read" "LINKER:--wrap=poll")
    endif()
    if(MINGW)
        target_link_options(logonomic_cli_addon_launcher_utilities_tests PRIVATE -municode)
    endif()
    if(_logonomic_cli_addon_sdk_top_level)
        enable_testing()
        if(CMAKE_GENERATOR STREQUAL "FASTBuild")
            add_test(NAME logonomic_cli_addon_sdk_fastbuild_environment
                COMMAND "${CMAKE_COMMAND}" "-DSDK_BUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fastbuild_environment.cmake")
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
            add_test(NAME logonomic_cli_addon_linux_runtime_tests
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_runtime_tests.py")
            set_tests_properties(logonomic_cli_addon_linux_runtime_tests PROPERTIES TIMEOUT 60)
            add_test(NAME logonomic_cli_addon_linux_runtime_recipe_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DSDK_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
                    "-DINDEX_TOOL=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                    "-DTEST_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/linux_runtime_recipe_tests"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_runtime_recipe_tests.cmake")
            set_tests_properties(logonomic_cli_addon_linux_runtime_recipe_tests PROPERTIES
                TIMEOUT 120 PROCESSORS 1)
        endif()
        add_test(NAME logonomic_cli_addon_launcher_utilities_tests
            COMMAND logonomic_cli_addon_launcher_utilities_tests)
        set_tests_properties(logonomic_cli_addon_launcher_utilities_tests PROPERTIES TIMEOUT 60)
    endif()

    add_executable(logonomic_cli_addon_sdk_tests
        tests/sdk_tests_main.cpp
        tests/sdk_test_support.cpp
        tests/json_tests.cpp
        tests/ids_tests.cpp
        tests/sha256_tests.cpp
        tests/validator_tests.cpp
        tests/frame_tests.cpp
        tests/protocol_tests.cpp
        tests/provider_event_diagnostic_tests.cpp
        tests/simulator_process_tests.cpp
        tests/conformance_tests.cpp
        tests/conversation_export_tests.cpp
        tests/context_state_tests.cpp
        tests/execution_context_tests.cpp
        tests/model_catalog_tests.cpp
        tests/cooperative_description_tests.cpp
        tests/model_state_tests.cpp
        tests/child_state_tests.cpp
        tests/capability_contract_tests.cpp
        tests/delegated_model_selection_tests.cpp
        tests/launcher_approval_tests.cpp
        tests/usage_session_tests.cpp
        tests/usage_repository_attribution_tests.cpp
        tests/usage_owned_event_correction_tests.cpp
        tests/interactive_initial_prompt_tests.cpp
        tests/terminal_codex_invocation_tests.cpp
        tests/session_wake_tests.cpp
        tests/terminal_identity_tests.cpp
        tests/accounts_tests.cpp
        tests/account_pet_tests.cpp
        tests/startup_observer_tests.cpp
        tests/session_observation_tests.cpp
        tests/lifecycle_correlation_tests.cpp
        tests/lifecycle_correlation_lifetime_tests.cpp
        tests/managed_configuration_tests.cpp
        tests/permission_snapshot_tests.cpp
        tests/environment_bootstrap_tests.cpp
        tests/observation_tests.cpp
        tests/quota_cost_tests.cpp
        tests/quota_failure_compatibility_tests.cpp)
    target_include_directories(logonomic_cli_addon_sdk_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(logonomic_cli_addon_sdk_tests PRIVATE logonomic_cli_addon_sdk::sdk)
    logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_sdk_tests)

    if(_logonomic_cli_addon_sdk_top_level)
        set(_logonomic_cli_addon_sdk_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixture_packages")
        logonomic_cli_addon_sdk_add_fixture_packages(
            OUTPUT_DIRECTORY "${_logonomic_cli_addon_sdk_fixture_dir}"
            TARGET           logonomic_cli_addon_sdk_fixture_packages)
        add_dependencies(logonomic_cli_addon_sdk_tests logonomic_cli_addon_sdk_fixture_packages)

        enable_testing()
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        add_test(NAME logonomic_cli_addon_observation_preparation_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/observation_preparation_tests.py")
        set_tests_properties(logonomic_cli_addon_observation_preparation_tests PROPERTIES TIMEOUT 30)
        add_test(NAME logonomic_cli_addon_macos_package_signing_tests
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_package_signing_tests.py")
        set_tests_properties(logonomic_cli_addon_macos_package_signing_tests PROPERTIES TIMEOUT 30)
        logonomic_cli_addon_sdk_test_groups(_logonomic_cli_addon_sdk_test_groups)
        add_test(NAME logonomic_cli_addon_sdk_tests
            COMMAND logonomic_cli_addon_sdk_tests
                --fixture-packages "${_logonomic_cli_addon_sdk_fixture_dir}"
                --scripts "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts"
                ${_logonomic_cli_addon_sdk_test_groups})
        set_tests_properties(logonomic_cli_addon_sdk_tests PROPERTIES TIMEOUT 240)
        # The package/tool conformance entries below target Windows and Linux.
        if(NOT APPLE)
            add_test(NAME logonomic_cli_addon_package_capabilities_tests
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_capabilities_tests.py"
                    "--cmake=${CMAKE_COMMAND}"
                    "--sdk-source=${CMAKE_CURRENT_SOURCE_DIR}"
                    "--index-tool=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "--validate-tool=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "--generator=${CMAKE_GENERATOR}"
                    "--make-program=${CMAKE_MAKE_PROGRAM}"
                    "--compiler=${CMAKE_CXX_COMPILER}"
                    "--toolchain=${CMAKE_TOOLCHAIN_FILE}"
                    "--generator-platform=${CMAKE_GENERATOR_PLATFORM}"
                    "--generator-toolset=${CMAKE_GENERATOR_TOOLSET}")
            set_tests_properties(logonomic_cli_addon_package_capabilities_tests PROPERTIES
                TIMEOUT 120 PROCESSORS 1)
            add_test(NAME logonomic_cli_addon_sdk_paced_progress_lifetime_tests
                COMMAND logonomic_cli_addon_sdk_tests
                    --fixture-packages "${_logonomic_cli_addon_sdk_fixture_dir}"
                    --scripts "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts"
                    fixture_paced_progress_lifetime)
            set_tests_properties(logonomic_cli_addon_sdk_paced_progress_lifetime_tests
                PROPERTIES TIMEOUT 120 LABELS slow)
            add_test(NAME logonomic_cli_addon_validate_cli_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cli_tests.cmake")
            set_tests_properties(logonomic_cli_addon_validate_cli_tests PROPERTIES TIMEOUT 30)
            add_test(NAME logonomic_cli_addon_simulator_deadline_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DSIMULATOR_TOOL=$<TARGET_FILE:logonomic_cli_addon_host_simulator>"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/simulator_deadline_tests"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/simulator_deadline_tests.cmake")
            set_tests_properties(logonomic_cli_addon_simulator_deadline_tests PROPERTIES TIMEOUT 60)
            add_test(NAME logonomic_cli_addon_stage_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DSTAGE_TOOL=$<TARGET_FILE:logonomic_cli_addon_stage>"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DINDEX_TOOL=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/stage_tests"
                    "-DCMAKE_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
                    "-DPACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
                    "-DPACKAGE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                    "-DPACKAGE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                    "-DPACKAGE_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/stage_tests.cmake")
            set_tests_properties(logonomic_cli_addon_stage_tests PROPERTIES TIMEOUT 120)
            add_test(NAME logonomic_cli_addon_fixture_conformance_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DCONFORMANCE_TOOL=$<TARGET_FILE:logonomic_cli_addon_conformance>"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DINDEX_TOOL=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/fixture_conformance_tests"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture_conformance_tests.cmake")
            set_tests_properties(logonomic_cli_addon_fixture_conformance_tests PROPERTIES TIMEOUT 60)
            add_test(NAME logonomic_cli_addon_session_declaration_conformance_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DCONFORMANCE_TOOL=$<TARGET_FILE:logonomic_cli_addon_conformance>"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DINDEX_TOOL=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/session_declaration_conformance_tests"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_declaration_conformance_tests.cmake")
            set_tests_properties(logonomic_cli_addon_session_declaration_conformance_tests PROPERTIES TIMEOUT 120)
            add_test(NAME logonomic_cli_addon_tool_unicode_paths
                COMMAND "${CMAKE_COMMAND}"
                    "-DINDEX_TOOL=$<TARGET_FILE:logonomic_cli_addon_index>"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DSIMULATOR_TOOL=$<TARGET_FILE:logonomic_cli_addon_host_simulator>"
                    "-DCONFORMANCE_TOOL=$<TARGET_FILE:logonomic_cli_addon_conformance>"
                    "-DFIXTURE_PACKAGES=${_logonomic_cli_addon_sdk_fixture_dir}"
                    "-DSCRIPT_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scripts"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tool_unicode_paths"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_unicode_paths.cmake")
            set_tests_properties(logonomic_cli_addon_tool_unicode_paths PROPERTIES TIMEOUT 120)

            # Builds the fixture package project on its own and embedded, one build
            # after the other, and compares the two packages. It acquires no CPUs
            # itself: the caller grants the width of one build, which PROCESSORS
            # declares.
            set(_logonomic_cli_addon_sdk_embedded_package_test_jobs 4)
            include("${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_package_cache.cmake")
            set(_logonomic_cli_addon_sdk_embedded_package_cache_file
                "${CMAKE_CURRENT_BINARY_DIR}/embedded_package_cache.cmake")
            logonomic_cli_addon_write_embedded_package_cache(
                "${_logonomic_cli_addon_sdk_embedded_package_cache_file}")
            set(_logonomic_cli_addon_sdk_fastbuild_cache OFF)
            if(CMAKE_FASTBUILD_CACHE_PATH OR NOT "$ENV{FASTBUILD_CACHE_PATH}" STREQUAL "")
                set(_logonomic_cli_addon_sdk_fastbuild_cache ON)
            endif()
            add_test(NAME logonomic_cli_addon_embedded_package_tests
                COMMAND "${CMAKE_COMMAND}"
                    "-DPACKAGE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_package"
                    "-DVALIDATE_TOOL=$<TARGET_FILE:logonomic_cli_addon_validate>"
                    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/embedded_package_tests"
                    "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
                    "-DTEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                    "-DTEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                    "-DTEST_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
                    "-DTEST_COMPILE_CACHE_FILE=${_logonomic_cli_addon_sdk_embedded_package_cache_file}"
                    "-DTEST_FASTBUILD_USE_CACHE=${_logonomic_cli_addon_sdk_fastbuild_cache}"
                    "-DTEST_PARALLEL_LEVEL=${_logonomic_cli_addon_sdk_embedded_package_test_jobs}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_package_tests.cmake")
            set_tests_properties(logonomic_cli_addon_embedded_package_tests PROPERTIES
                TIMEOUT 1200
                PROCESSORS ${_logonomic_cli_addon_sdk_embedded_package_test_jobs})
        endif()
    endif()
endif()
