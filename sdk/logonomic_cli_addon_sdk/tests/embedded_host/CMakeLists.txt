cmake_minimum_required(VERSION 3.21)
# As at Logonomic's top level: with the MSVC debug-information format an
# abstraction, a package's CMAKE_MSVC_DEBUG_INFORMATION_FORMAT applies to its
# targets embedded as it does on its own.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
# The languages Logonomic's build enables, so that the cache a package finds
# here holds what it would find there.
project(logonomic_cli_addon_embedded_host LANGUAGES C CXX)

# The smallest embedding build: it adds the SDK once and then the package
# project PACKAGE_SOURCE as a subproject, with LOGONOMIC_CLI_ADDON_EMBEDDED
# set, the way an application's build adds each package it stages
# (docs/package_contract.md, "Embedded build"). It is the embedded half of
# tests/embedded_package_tests.cmake, which builds the same package on its own
# as well and compares the two.

if(NOT DEFINED PACKAGE_SOURCE OR NOT EXISTS "${PACKAGE_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR
        "PACKAGE_SOURCE must name a package project directory: '${PACKAGE_SOURCE}'")
endif()

# The SDK is compiled as it compiles itself on its own, which is how the
# maintained packages compile it too, so that a difference between the two
# packages is the package's and not this host's.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." sdk)

# Every cache entry as <name>=<digest of its type, value and help string>.
function(logonomic_cli_addon_embedded_host_cache_state out_state)
    get_cmake_property(names CACHE_VARIABLES)
    set(state)
    foreach(name IN LISTS names)
        get_property(type CACHE "${name}" PROPERTY TYPE)
        get_property(value CACHE "${name}" PROPERTY VALUE)
        get_property(help CACHE "${name}" PROPERTY HELPSTRING)
        string(SHA256 digest "${type}\n${value}\n${help}")
        list(APPEND state "${name}=${digest}")
    endforeach()
    set(${out_state} "${state}" PARENT_SCOPE)
endfunction()

# The build properties of this directory, the one that adds the package, as
# <property>=<digest of its value>: the settings every target the host adds
# after the package starts from.
function(logonomic_cli_addon_embedded_host_directory_state out_state)
    set(state)
    foreach(property IN ITEMS
        COMPILE_OPTIONS
        COMPILE_DEFINITIONS
        INCLUDE_DIRECTORIES
        LINK_OPTIONS
        LINK_DIRECTORIES
        LINK_LIBRARIES)
        get_directory_property(value ${property})
        string(SHA256 digest "${value}")
        list(APPEND state "${property}=${digest}")
    endforeach()
    set(${out_state} "${state}" PARENT_SCOPE)
endfunction()

# Writes <file> with one "added", "changed" or "removed" line for each name
# whose digest differs between two <name>=<digest> states, for the comparison
# to judge.
function(logonomic_cli_addon_embedded_host_write_changes before after file)
    list(TRANSFORM before REPLACE "=.*$" "" OUTPUT_VARIABLE names_before)
    list(TRANSFORM after REPLACE "=.*$" "" OUTPUT_VARIABLE names_after)
    set(changes)
    foreach(entry IN LISTS after)
        if(entry IN_LIST before)
            continue()
        endif()
        string(REGEX REPLACE "=.*$" "" name "${entry}")
        if(name IN_LIST names_before)
            list(APPEND changes "changed ${name}")
        else()
            list(APPEND changes "added ${name}")
        endif()
    endforeach()
    foreach(name IN LISTS names_before)
        if(NOT name IN_LIST names_after)
            list(APPEND changes "removed ${name}")
        endif()
    endforeach()
    list(JOIN changes "\n" changes)
    file(WRITE "${file}" "${changes}")
endfunction()

# A normal variable of this function's scope, never a cache entry: it reaches
# the package's directory scope and nothing the host adds outside it.
function(logonomic_cli_addon_embedded_host_add_package source_directory)
    set(LOGONOMIC_CLI_ADDON_EMBEDDED ON)
    add_subdirectory("${source_directory}" embedded)
endfunction()

logonomic_cli_addon_embedded_host_cache_state(cache_before)
logonomic_cli_addon_embedded_host_directory_state(directory_before)
logonomic_cli_addon_embedded_host_add_package("${PACKAGE_SOURCE}")
logonomic_cli_addon_embedded_host_cache_state(cache_after)
logonomic_cli_addon_embedded_host_directory_state(directory_after)
logonomic_cli_addon_embedded_host_write_changes("${cache_before}" "${cache_after}"
    "${CMAKE_BINARY_DIR}/embedded-cache-changes.txt")
logonomic_cli_addon_embedded_host_write_changes("${directory_before}" "${directory_after}"
    "${CMAKE_BINARY_DIR}/embedded-directory-changes.txt")

# The registrations as the package made them, and with each configuration's
# directory resolved, for the comparison to read.
get_property(registrations GLOBAL PROPERTY LOGONOMIC_CLI_ADDON_EMBEDDED_PACKAGES)
file(WRITE "${CMAKE_BINARY_DIR}/embedded-packages.txt" "${registrations}")
file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/embedded-packages-$<CONFIG>.txt"
    CONTENT "${registrations}")
