cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(../sdk/logonomic_cli_addon_sdk/cmake/standalone_toolchain.cmake NO_POLICY_SCOPE)
    project(logonomic_cli_addon_host LANGUAGES CXX)
    enable_testing()
    add_subdirectory(../sdk/logonomic_cli_addon_sdk sdk)
endif()

include(FetchContent)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)
find_package(Threads REQUIRED)
if(NOT TARGET vnm_process_custody::vnm_process_custody)
    FetchContent_Declare(vnm_process_custody
        GIT_REPOSITORY https://github.com/Varinomics/vnm_process_custody.git
        GIT_TAG master)
    FetchContent_MakeAvailable(vnm_process_custody)
endif()
if(NOT TARGET vnm_qt_dispatch::vnm_qt_dispatch)
    FetchContent_Declare(vnm_qt_dispatch
        GIT_REPOSITORY https://github.com/Varinomics/vnm_qt_dispatch.git
        GIT_TAG master)
    FetchContent_MakeAvailable(vnm_qt_dispatch)
endif()

add_library(logonomic_cli_addon_host_headers INTERFACE)
target_include_directories(logonomic_cli_addon_host_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(logonomic_cli_addon_process_support STATIC
    src/logonomic_cli_addon_process_drain.cpp
    src/logonomic_cli_addon_usage_writer_custody.cpp)
if(WIN32)
    target_sources(logonomic_cli_addon_process_support PRIVATE src/logonomic_cli_addon_generation_custody_win.cpp)
    target_compile_definitions(logonomic_cli_addon_process_support PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
else()
    target_sources(logonomic_cli_addon_process_support PRIVATE src/logonomic_cli_addon_generation_custody_posix.cpp)
endif()
target_link_libraries(logonomic_cli_addon_process_support PUBLIC
    logonomic_cli_addon_host_headers logonomic_cli_addon_sdk::sdk vnm_process_custody::vnm_process_custody)

add_executable(logonomic_cli_addon_custodian src/logonomic_cli_addon_custodian_main.cpp)
target_link_libraries(logonomic_cli_addon_custodian PRIVATE logonomic_cli_addon_process_support)
if(APPLE)
    vnm_process_custody_deploy_owner(logonomic_cli_addon_custodian)
endif()
add_executable(logonomic_cli_addon_supervisor src/logonomic_cli_addon_supervisor_main.cpp)
target_link_libraries(logonomic_cli_addon_supervisor PRIVATE logonomic_cli_addon_process_support)
if(MINGW)
    target_link_options(logonomic_cli_addon_supervisor PRIVATE -municode)
endif()

add_library(logonomic_cli_addon_registry STATIC
    src/logonomic_cli_addon_generation_custody.cpp
    src/logonomic_cli_addon_json_bridge.cpp
    src/logonomic_cli_addon_registry.cpp
    src/logonomic_cli_addon_roots.cpp)
target_link_libraries(logonomic_cli_addon_registry PUBLIC logonomic_cli_addon_process_support Qt6::Core)

function(logonomic_add_generation_host target)
    add_library(${target} STATIC
        src/logonomic_cli_addon_generation_client.cpp
        src/logonomic_cli_addon_generation_client.h
        src/logonomic_cli_addon_request_broker.cpp
        src/logonomic_cli_addon_request_broker.h)
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
    target_link_libraries(${target}
        PUBLIC logonomic_cli_addon_registry
        PRIVATE vnm_qt_dispatch::vnm_qt_dispatch Threads::Threads)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
    endif()
endfunction()
logonomic_add_generation_host(logonomic_cli_addon_host)

option(LOGONOMIC_CLI_ADDON_HOST_TEST_HOOKS "Build the host variant used for desktop fault injection" OFF)
if(LOGONOMIC_CLI_ADDON_HOST_TEST_HOOKS)
    logonomic_add_generation_host(logonomic_cli_addon_host_test)
    target_compile_definitions(logonomic_cli_addon_host_test PUBLIC LOGONOMIC_ENABLE_CLI_PROVIDER_TEST_HOOKS)
    target_compile_definitions(logonomic_cli_addon_registry PRIVATE LOGONOMIC_ENABLE_CLI_ADDON_CUSTODY_TEST_HOOKS)
endif()
set_target_properties(logonomic_cli_addon_process_support logonomic_cli_addon_custodian
    logonomic_cli_addon_supervisor logonomic_cli_addon_registry PROPERTIES AUTOMOC OFF)
if(WIN32)
    foreach(target IN ITEMS logonomic_cli_addon_process_support logonomic_cli_addon_custodian
            logonomic_cli_addon_supervisor logonomic_cli_addon_registry)
        target_compile_definitions(${target} PRIVATE NOMINMAX)
    endforeach()
endif()

option(LOGONOMIC_CLI_ADDON_HOST_BUILD_TESTS "Build the native host integration tests" ${PROJECT_IS_TOP_LEVEL})
if(LOGONOMIC_CLI_ADDON_HOST_BUILD_TESTS)
    set(host_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>")
    logonomic_cli_addon_sdk_add_fixture_packages(
        OUTPUT_DIRECTORY "${host_fixture_root}"
        TARGET logonomic_cli_addon_host_fixture_packages
        PACKAGE_IDS
            org.example.unknown-fixture
            org.example.unknown-fixture-provider-surfaces
            org.example.unknown-fixture-hang-on-invoke)

    function(logonomic_register_native_host_test name target)
        add_test(NAME ${name}
            COMMAND ${target} "${host_fixture_root}"
                $<TARGET_FILE:logonomic_cli_addon_custodian> $<TARGET_FILE:logonomic_cli_addon_supervisor> ${ARGN})
        set_tests_properties(${name} PROPERTIES TIMEOUT 45)
        if(WIN32)
            set_property(TEST ${name} PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endfunction()
    function(logonomic_add_native_host_test target source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE logonomic_cli_addon_host)
        add_dependencies(${target} logonomic_cli_addon_host_fixture_packages
            logonomic_cli_addon_custodian logonomic_cli_addon_supervisor)
        logonomic_register_native_host_test(${target} ${target})
    endfunction()
    logonomic_add_native_host_test(logonomic_cli_addon_generation_tests tests/generation_tests.cpp)
    foreach(scenario IN ITEMS forced fault shutdown unsolicited_exit)
        logonomic_register_native_host_test(logonomic_cli_addon_generation_idle_${scenario}
            logonomic_cli_addon_generation_tests ${scenario})
    endforeach()
    logonomic_add_native_host_test(logonomic_cli_addon_request_broker_tests tests/logonomic_cli_addon_request_broker_tests.cpp)
endif()
