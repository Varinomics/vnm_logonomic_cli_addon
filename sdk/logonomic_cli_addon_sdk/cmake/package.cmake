# These helpers are available to source consumers without raising the SDK's
# minimum CMake version. Package target proof requires CMake 3.25 when invoked.
function(logonomic_cli_addon_add_launch_bridge target)
    add_executable(${target}
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/logonomic_cli_addon_launch_bridge.cpp")
    # The bridge's Windows entry point is wmain, which MinGW links only with
    # -municode, as it does the SDK's own tools.
    if(MINGW)
        target_link_options(${target} PRIVATE -municode)
    endif()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/qt_runtime.cmake")

function(logonomic_cli_addon_macos_runtime_deployer out_script)
    set(${out_script}
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_macos_runtime.py"
        PARENT_SCOPE)
endfunction()

function(logonomic_cli_addon_linux_runtime_deployer out_script)
    set(${out_script}
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_linux_runtime.py"
        PARENT_SCOPE)
endfunction()

# Return the complete command and file dependencies so a signature change
# rebuilds the package before its integrity index seals the final bytes.
function(logonomic_cli_addon_package_indexer out_command out_dependencies)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "LINUX_RUNTIME" "RUNTIME_NOTICES" "CONTROLLED_TARGETS")
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Package indexer requires complete named arguments.")
    endif()
    if((ARG_RUNTIME_NOTICES OR ARG_CONTROLLED_TARGETS) AND NOT ARG_LINUX_RUNTIME)
        message(FATAL_ERROR "Runtime notices and controlled targets require LINUX_RUNTIME deployment.")
    endif()
    if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
        set(LOGONOMIC_CLI_ADDON_MACOS_SIGNING_IDENTITY "" CACHE STRING
            "Developer ID Application identity for production macOS packages")
        set(LOGONOMIC_CLI_ADDON_MACOS_TEAM_ID "" CACHE STRING
            "Apple team ID for production macOS packages")
    endif()
    set(command "$<TARGET_FILE:logonomic_cli_addon_index>")
    set(dependencies)
    if(ARG_LINUX_RUNTIME)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "Linux runtime deployment requires a Linux target.")
        endif()
        if(NOT ARG_CONTROLLED_TARGETS)
            message(FATAL_ERROR "Linux runtime deployment requires its controlled executable targets.")
        endif()
        foreach(target IN LISTS ARG_CONTROLLED_TARGETS)
            get_target_property(target_type "${target}" TYPE)
            if(NOT target_type STREQUAL "EXECUTABLE")
                message(FATAL_ERROR "Linux runtime controlled target '${target}' must be an executable.")
            endif()
            target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-g0>")
            target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:LINKER:--strip-all>")
        endforeach()
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        find_program(_logonomic_cli_addon_patchelf patchelf REQUIRED)
        find_program(_logonomic_cli_addon_readelf readelf NO_CACHE)
        if(NOT _logonomic_cli_addon_readelf)
            message(FATAL_ERROR "Linux package assembly requires readelf. Install binutils in the builder.")
        endif()
        set(deployer "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_linux_runtime.py")
        set(command "${Python3_EXECUTABLE}" -B "${deployer}" deploy
            --configuration "$<CONFIG>" --patchelf "${_logonomic_cli_addon_patchelf}"
            --index "$<TARGET_FILE:logonomic_cli_addon_index>")
        if(ARG_RUNTIME_NOTICES)
            list(APPEND command --runtime-notices "${ARG_RUNTIME_NOTICES}")
        endif()
        list(APPEND command --)
        list(APPEND dependencies "${deployer}")
    endif()
    if(NOT "${LOGONOMIC_CLI_ADDON_MACOS_SIGNING_IDENTITY}" STREQUAL "" OR
       NOT "${LOGONOMIC_CLI_ADDON_MACOS_TEAM_ID}" STREQUAL "")
        if("${LOGONOMIC_CLI_ADDON_MACOS_SIGNING_IDENTITY}" STREQUAL "" OR
           "${LOGONOMIC_CLI_ADDON_MACOS_TEAM_ID}" STREQUAL "")
            message(FATAL_ERROR "macOS package signing requires both signing identity and team ID.")
        endif()
        if(NOT APPLE)
            message(FATAL_ERROR "macOS package signing requires an Apple target.")
        endif()
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        set(signer "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sign_macos_package.py")
        set(command "${Python3_EXECUTABLE}" -B "${signer}"
            --signing-identity "${LOGONOMIC_CLI_ADDON_MACOS_SIGNING_IDENTITY}"
            --team-id "${LOGONOMIC_CLI_ADDON_MACOS_TEAM_ID}"
            --configuration "$<CONFIG>"
            --index "$<TARGET_FILE:logonomic_cli_addon_index>" --)
        set(dependencies "${signer}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_macos_runtime.py")
    endif()
    set(${out_command} "${command}" PARENT_SCOPE)
    set(${out_dependencies} "${dependencies}" PARENT_SCOPE)
endfunction()

function(logonomic_cli_addon_package_platform out_platform)
    if(CMAKE_VERSION VERSION_LESS 3.25)
        message(FATAL_ERROR "Package compiler-target proof requires CMake 3.25 or newer.")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(platform windows-x86_64)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(platform linux-x86_64)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(platform macos-arm64)
        if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
            message(FATAL_ERROR "macOS package artifacts require a single arm64 target.")
        endif()
    else()
        message(FATAL_ERROR "Package artifacts support Windows/Linux x86_64 and macOS arm64.")
    endif()

    # The probe must not link or run target code, or capture the configure
    # environment in a nested FASTBuild project.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
    list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
    if(platform STREQUAL "macos-arm64")
        set(target_source [=[
#if !defined(__APPLE__) || !defined(__aarch64__)
#error Package artifacts require an Apple arm64 compiler target.
#endif
static_assert(sizeof(void*) == 8, "Package artifacts require the 64-bit arm64 ABI.");
]=])
    else()
        set(target_source [=[
#if !(defined(_M_X64) || defined(__x86_64__)) || defined(_M_ARM64EC) || defined(__arm64ec__)
#error Package artifacts require an x86_64 compiler target.
#endif
static_assert(sizeof(void*) == 8, "Package artifacts require the 64-bit x86_64 ABI.");
]=])
    endif()
    # Include the single-config build type (possibly empty) as well as every
    # generated configuration: per-configuration flags may change the ABI.
    foreach(configuration IN ITEMS "${CMAKE_BUILD_TYPE}" ${CMAKE_CONFIGURATION_TYPES})
        set(CMAKE_TRY_COMPILE_CONFIGURATION "${configuration}")
        try_compile(target_supported
            SOURCE_FROM_CONTENT package_target.cpp "${target_source}"
            NO_CACHE
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            OUTPUT_VARIABLE target_diagnostic)
        if(NOT target_supported)
            message(FATAL_ERROR
                "Cannot prove the package compiler target is ${platform} "
                "for configuration '${configuration}':\n${target_diagnostic}")
        endif()
    endforeach()
    set(${out_platform} "${platform}" PARENT_SCOPE)
endfunction()

# The package project's own binary directory is the build root when the
# package is built on its own, and the package's subdirectory when an
# embedding build adds it, so packages embedded side by side never share one.
function(logonomic_cli_addon_package_directory package_id out_directory)
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(directory "${PROJECT_BINARY_DIR}/package/$<CONFIG>/${package_id}")
    else()
        set(directory "${PROJECT_BINARY_DIR}/package/${package_id}")
    endif()
    set(${out_directory} "${directory}" PARENT_SCOPE)
endfunction()

# Assemble one standard package in the calling project. The caller configures
# generated/logonomic-cli-addon.json and supplies native executable targets;
# Present files under package/{assets,capabilities,provenance,notices} are copied without filtering.
# Adapter/helper filenames retain the target name plus CMAKE_EXECUTABLE_SUFFIX.
# QtCore recipes use logonomic_cli_addon_add_qt_package for their runtime stages.
#
# logonomic_cli_addon_add_package(<target> <package_id> <out_directory>
#     ADAPTER <target> [HELPERS <targets>...] [INDEX_ARGUMENTS <arguments>...]
#     [CAPABILITIES_DIRECTORY <absolute-directory>])
function(logonomic_cli_addon_add_package target package_id out_directory)
    cmake_parse_arguments(PARSE_ARGV 3 ARG "" "ADAPTER;CAPABILITIES_DIRECTORY" "HELPERS;INDEX_ARGUMENTS")
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES OR
       NOT DEFINED ARG_ADAPTER OR ARG_ADAPTER STREQUAL "")
        message(FATAL_ERROR
            "logonomic_cli_addon_add_package requires ADAPTER and complete named arguments.")
    endif()
    set(capabilities_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities")
    if(DEFINED ARG_CAPABILITIES_DIRECTORY)
        if(NOT IS_ABSOLUTE "${ARG_CAPABILITIES_DIRECTORY}" OR
           NOT IS_DIRECTORY "${ARG_CAPABILITIES_DIRECTORY}")
            message(FATAL_ERROR "CAPABILITIES_DIRECTORY must be an existing absolute directory at configure time.")
        endif()
        set(capabilities_directory "${ARG_CAPABILITIES_DIRECTORY}")
    endif()
    logonomic_cli_addon_package_directory("${package_id}" package_directory)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        logonomic_cli_addon_package_indexer(package_index_command package_index_dependencies
            LINUX_RUNTIME CONTROLLED_TARGETS ${ARG_ADAPTER} ${ARG_HELPERS})
    else()
        logonomic_cli_addon_package_indexer(package_index_command package_index_dependencies)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        # Build ownership is independent of any third-party code linked into
        # the adapter or helpers. Record the actual targets before indexing.
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        logonomic_cli_addon_macos_runtime_deployer(macos_deployer)
        set(macos_arguments --package "${package_directory}" --configuration "$<CONFIG>")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            list(APPEND macos_arguments --minimum-macos "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        set(adapter_relative "adapter/${ARG_ADAPTER}${CMAKE_EXECUTABLE_SUFFIX}")
        list(APPEND macos_arguments --input "$<TARGET_FILE:${ARG_ADAPTER}>" "${adapter_relative}"
            --executable "${adapter_relative}")
        foreach(helper IN LISTS ARG_HELPERS)
            set(helper_relative "helpers/${helper}${CMAKE_EXECUTABLE_SUFFIX}")
            list(APPEND macos_arguments --input "$<TARGET_FILE:${helper}>" "${helper_relative}"
                --executable "${helper_relative}")
        endforeach()
        list(APPEND package_index_dependencies "${macos_deployer}")
    endif()
    set(package_files)
    set(copy_directories)
    set(input_directories assets capabilities provenance notices)
    set(input_command "${CMAKE_COMMAND}")
    set(input_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_inputs.cmake")
    set(input_stamp "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt")
    foreach(directory IN LISTS input_directories)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/package/${directory}")
        if(directory STREQUAL "capabilities")
            set(source "${capabilities_directory}")
        endif()
        if((EXISTS "${source}" OR IS_SYMLINK "${source}") AND NOT IS_DIRECTORY "${source}")
            message(FATAL_ERROR "Expected a package input directory at ${source}")
        endif()
        if(CMAKE_GENERATOR STREQUAL "FASTBuild")
            list(APPEND input_command "-Dsource_${directory}=${source}")
        else()
            file(GLOB_RECURSE inputs CONFIGURE_DEPENDS "${source}/*")
            if(inputs)
                list(APPEND package_files ${inputs})
                list(APPEND copy_directories COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${source}" "${package_directory}/${directory}")
            endif()
        endif()
    endforeach()
    if(CMAKE_GENERATOR STREQUAL "FASTBuild")
        # CMake 4.4 can acknowledge a glob change without regenerating its
        # FASTBuild graph. Check the selected files before evaluating the index.
        add_custom_target(${target}_inputs
            COMMAND ${input_command} "-Ddirectories=${input_directories}"
                "-Dstamp=${input_stamp}" -P "${input_script}"
            BYPRODUCTS "${input_stamp}"
            VERBATIM)
        list(APPEND package_index_dependencies "${input_script}")
    else()
        file(GENERATE OUTPUT "${input_stamp}" CONTENT "${capabilities_directory}\n${package_files}\n")
    endif()
    add_custom_command(
        OUTPUT "${package_directory}/package.files.json"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${package_directory}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${package_directory}/adapter" "${package_directory}/helpers"
        ${copy_directories}
        COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${package_directory}/logonomic-cli-addon.json"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${ARG_ADAPTER}>"
            "${package_directory}/adapter/${ARG_ADAPTER}${CMAKE_EXECUTABLE_SUFFIX}"
        DEPENDS ${ARG_ADAPTER} ${ARG_HELPERS} logonomic_cli_addon_index
            "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${input_stamp}"
            ${package_files} ${package_index_dependencies}
        VERBATIM)
    if(CMAKE_GENERATOR STREQUAL "FASTBuild")
        add_custom_command(OUTPUT "${package_directory}/package.files.json" APPEND
            COMMAND ${input_command} "-Ddirectories=${input_directories}"
                "-Ddestination=${package_directory}" -P "${input_script}")
    endif()
    foreach(helper IN LISTS ARG_HELPERS)
        add_custom_command(OUTPUT "${package_directory}/package.files.json" APPEND
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${helper}>"
                "${package_directory}/helpers/${helper}${CMAKE_EXECUTABLE_SUFFIX}")
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_custom_command(OUTPUT "${package_directory}/package.files.json" APPEND
            COMMAND "${Python3_EXECUTABLE}" -B "${macos_deployer}" ${macos_arguments})
    endif()
    add_custom_command(OUTPUT "${package_directory}/package.files.json" APPEND
        COMMAND ${package_index_command} "${package_directory}" ${ARG_INDEX_ARGUMENTS})
    add_custom_target(${target} ALL DEPENDS "${package_directory}/package.files.json")
    if(CMAKE_GENERATOR STREQUAL "FASTBuild")
        add_dependencies(${target} ${target}_inputs)
    endif()
    logonomic_cli_addon_install_package("${package_directory}" "${package_id}" TARGET ${target})
    set(${out_directory} "${package_directory}" PARENT_SCOPE)
endfunction()

# logonomic_cli_addon_install_package(<package_directory> <package_id>
#                                     [TARGET <assembly target>])
#
# On its own, a package exports a fresh artifact at install time and TARGET is
# ignored. An embedding build stages its packages itself: it sets the normal
# variable LOGONOMIC_CLI_ADDON_EMBEDDED before it adds a package, and then the
# package adds no install rules and is registered instead, as
# `<package_id>|<package_directory>|<assembly target>` in the global property
# LOGONOMIC_CLI_ADDON_EMBEDDED_PACKAGES (docs/package_contract.md, "Embedded
# build").
function(logonomic_cli_addon_install_package package_directory package_id)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "TARGET" "")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "logonomic_cli_addon_install_package got unexpected arguments: "
            "${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(LOGONOMIC_CLI_ADDON_EMBEDDED)
        if(NOT DEFINED ARG_TARGET)
            message(FATAL_ERROR
                "Embedded package ${package_id} must name the target that "
                "assembles it: logonomic_cli_addon_install_package("
                "<directory> ${package_id} TARGET <assembly target>).")
        endif()
        if(NOT TARGET "${ARG_TARGET}")
            message(FATAL_ERROR
                "Embedded package ${package_id} names '${ARG_TARGET}' as its "
                "assembly target, and no such target is defined before the call.")
        endif()
        set_property(GLOBAL APPEND PROPERTY LOGONOMIC_CLI_ADDON_EMBEDDED_PACKAGES
            "${package_id}|${package_directory}|${ARG_TARGET}")
        return()
    endif()

    # Prefix and DESTDIR belong to the install invocation, including --prefix.
    # This exports a fresh artifact; the application's installer owns updates.
    set(destination_check [=[
set(package_prefix "${CMAKE_INSTALL_PREFIX}")
set(package_destdir "$ENV{DESTDIR}")
string(REPLACE "\\" "/" package_prefix "${package_prefix}")
string(REPLACE "\\" "/" package_destdir "${package_destdir}")
if(NOT package_destdir STREQUAL "" AND package_prefix MATCHES "^[A-Za-z]:")
    message(FATAL_ERROR "Package export does not support DESTDIR with a drive-letter install prefix.")
endif()
string(REGEX REPLACE "/+$" "" package_destdir "${package_destdir}")
set(package_destination "${package_destdir}${package_prefix}/@package_id@")
if(EXISTS "${package_destination}" OR IS_SYMLINK "${package_destination}")
    message(FATAL_ERROR
        "Package export requires an absent destination: ${package_destination}. "
        "Use a fresh artifact destination; application package ingestion owns installed-generation updates.")
endif()
]=])
    string(CONFIGURE "${destination_check}" destination_check @ONLY)
    install(CODE "${destination_check}")
    install(DIRECTORY "${package_directory}/"
        DESTINATION "${package_id}" USE_SOURCE_PERMISSIONS)
endfunction()
