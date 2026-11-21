include_guard(GLOBAL)

# A QtCore-based package owns its programs and resources; the SDK owns their
# shared-runtime closure, notices and final indexing. No tests are registered
# here so an embedded package leaves the host's testing policy untouched.
#
# logonomic_cli_addon_add_qt_package(<target> <package_id> <out_directory>
#     ADAPTER <target> [HELPERS <targets>...]
#     [QTBASE_SOURCE <directory>] [RUNTIME_NOTICES <directory>]
#     [LINUX_RUNTIME_MATERIAL <file>] [PATCHELF <executable>]
#     [EXTRA_FILES <source> <package-relative-destination>...]
#     [OUT_RUNTIME_CHECK <out_command>] [OUT_QTBASE_SOURCE <out_directory>]
#     [INDEX_ARGUMENTS <arguments>...])
function(logonomic_cli_addon_add_qt_package target package_id out_directory)
    cmake_parse_arguments(PARSE_ARGV 3 ARG ""
        "ADAPTER;QTBASE_SOURCE;RUNTIME_NOTICES;LINUX_RUNTIME_MATERIAL;PATCHELF;OUT_RUNTIME_CHECK;OUT_QTBASE_SOURCE"
        "HELPERS;EXTRA_FILES;INDEX_ARGUMENTS")
    set(missing_arguments "${ARG_KEYWORDS_MISSING_VALUES}")
    list(REMOVE_ITEM missing_arguments QTBASE_SOURCE RUNTIME_NOTICES LINUX_RUNTIME_MATERIAL PATCHELF)
    if(ARG_UNPARSED_ARGUMENTS OR missing_arguments OR NOT ARG_ADAPTER)
        message(FATAL_ERROR
            "logonomic_cli_addon_add_qt_package requires ADAPTER and complete named arguments.")
    endif()
    if(NOT TARGET Qt6::Core)
        message(FATAL_ERROR "Qt package assembly requires the selected Qt6::Core target.")
    endif()
    foreach(program IN ITEMS ${ARG_ADAPTER} ${ARG_HELPERS})
        get_target_property(program_type "${program}" TYPE)
        if(NOT program_type STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "Qt package program '${program}' must be an executable target.")
        endif()
    endforeach()

    logonomic_cli_addon_package_directory("${package_id}" qt_package_directory)
    logonomic_cli_addon_package_indexer(qt_package_indexer qt_index_dependencies)
    set(qt_runtime_directory "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set(qt_recipe_directory "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(qtbase_source "${ARG_QTBASE_SOURCE}")
    set(runtime_notices_directory "${ARG_RUNTIME_NOTICES}")
    set(linux_runtime_material "${ARG_LINUX_RUNTIME_MATERIAL}")
    set(qt_index_arguments "${ARG_INDEX_ARGUMENTS}")
    set(qt_extra_copies "")
    list(LENGTH ARG_EXTRA_FILES extra_file_count)
    math(EXPR incomplete_extra_file "${extra_file_count} % 2")
    if(incomplete_extra_file)
        message(FATAL_ERROR "EXTRA_FILES requires source and package-relative destination pairs.")
    endif()
    while(ARG_EXTRA_FILES)
        list(POP_FRONT ARG_EXTRA_FILES input relative)
        string(REPLACE "\\" "/" relative "${relative}")
        if(NOT relative OR IS_ABSOLUTE "${relative}" OR relative MATCHES "(^|/)\\.\\.(/|$)")
            message(FATAL_ERROR "EXTRA_FILES destination must stay inside the package: ${relative}")
        endif()
        get_filename_component(input "${input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND qt_extra_copies "${input}|${relative}")
    endwhile()

    get_target_property(qmake Qt6::qmake IMPORTED_LOCATION)
    if(NOT qmake)
        get_target_property(qmake_configurations Qt6::qmake IMPORTED_CONFIGURATIONS)
        list(GET qmake_configurations 0 qmake_configuration)
        get_target_property(qmake Qt6::qmake "IMPORTED_LOCATION_${qmake_configuration}")
    endif()
    execute_process(COMMAND "${qmake}" -query QT_INSTALL_PREFIX
        OUTPUT_VARIABLE qt_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    if(NOT qtbase_source AND EXISTS "${qt_prefix}/../Src/qtbase/LICENSES")
        get_filename_component(qtbase_source "${qt_prefix}/../Src/qtbase" ABSOLUTE)
    endif()
    if(NOT qtbase_source)
        # The tag identifies the selected runtime's notice material. It places
        # no ceiling on the Qt versions a consumer may select.
        include(FetchContent)
        FetchContent_Declare(logonomic_cli_addon_qt_notices
            GIT_REPOSITORY https://code.qt.io/qt/qtbase.git
            GIT_TAG "v${Qt6Core_VERSION}"
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR _notices_only)
        FetchContent_MakeAvailable(logonomic_cli_addon_qt_notices)
        set(qtbase_source "${logonomic_cli_addon_qt_notices_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${qtbase_source}/LICENSES/LGPL-3.0-only.txt")
        message(FATAL_ERROR "QTBASE_SOURCE must provide the selected Qt runtime's qtbase notices.")
    endif()
    file(STRINGS "${qtbase_source}/.cmake.conf" qt_source_version
        REGEX [=[^set\(QT_REPO_MODULE_VERSION ]=])
    if(NOT qt_source_version MATCHES "\"${Qt6Core_VERSION}\"")
        message(FATAL_ERROR "QTBASE_SOURCE must describe the selected Qt ${Qt6Core_VERSION} runtime.")
    endif()
    file(GLOB_RECURSE qt_notice_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${qtbase_source}/LICENSES/*.txt"
        "${qtbase_source}/src/3rdparty/*LICENSE*"
        "${qtbase_source}/src/3rdparty/*LICENCE*"
        "${qtbase_source}/src/3rdparty/*COPYING*"
        "${qtbase_source}/src/3rdparty/*COPYRIGHT*"
        "${qtbase_source}/src/3rdparty/*qt_attribution*.json")
    file(GLOB qt_sbom_inputs "${qt_prefix}/sbom/qtbase-*.spdx")
    list(APPEND qt_notice_inputs ${qt_sbom_inputs})
    set(qt_runtime_search_dirs "$<TARGET_FILE_DIR:Qt6::Core>")
    set(qt_compiler_runtime_files "")
    set(qt_debug_runtime_files "")
    if(MSVC)
        # Resolve redistributables from the selected compiler rather than PATH.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
        set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
        set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
        include(InstallRequiredSystemLibraries)
        set(qt_compiler_runtime_files "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
        set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
        set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
        include(InstallRequiredSystemLibraries)
        set(qt_debug_runtime_files "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
        foreach(runtime IN LISTS qt_compiler_runtime_files qt_debug_runtime_files)
            get_filename_component(runtime_directory "${runtime}" DIRECTORY)
            list(APPEND qt_runtime_search_dirs "${runtime_directory}")
        endforeach()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
        logonomic_cli_addon_linux_runtime_deployer(linux_runtime_deployer)
        if(ARG_PATCHELF)
            set(patchelf_executable "${ARG_PATCHELF}")
        else()
            find_program(patchelf_executable patchelf REQUIRED NO_CACHE)
        endif()
        foreach(program IN ITEMS ${ARG_ADAPTER} ${ARG_HELPERS})
            # Build-tree executables retain their selected Qt path until the
            # assembler replaces this exact RPATH with the package-relative one.
            set_target_properties(${program} PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN;$<TARGET_FILE_DIR:Qt6::Core>"
                INSTALL_RPATH_USE_LINK_PATH FALSE)
            target_link_options(${program} PRIVATE "LINKER:--disable-new-dtags")
            target_compile_options(${program} PRIVATE "$<$<CONFIG:Release>:-g0>")
            target_link_options(${program} PRIVATE "$<$<CONFIG:Release>:LINKER:--strip-all>")
        endforeach()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
        logonomic_cli_addon_macos_runtime_deployer(macos_runtime_deployer)
        foreach(program IN ITEMS ${ARG_ADAPTER} ${ARG_HELPERS})
            target_link_options(${program} PRIVATE "LINKER:-headerpad_max_install_names")
        endforeach()
    endif()

    set(qt_adapter_file "$<TARGET_FILE:${ARG_ADAPTER}>")
    set(qt_executables "${qt_adapter_file}")
    set(qt_executable_relatives "adapter/$<TARGET_FILE_NAME:${ARG_ADAPTER}>")
    foreach(helper IN LISTS ARG_HELPERS)
        list(APPEND qt_executables "$<TARGET_FILE:${helper}>")
        list(APPEND qt_executable_relatives "adapter/$<TARGET_FILE_NAME:${helper}>")
    endforeach()
    list(LENGTH qt_executables qt_executable_count)
    if(WIN32)
        # Native cache paths become CMake source during template substitution;
        # normalize their separators without changing caller-owned cache values.
        foreach(path_variable IN ITEMS
                qt_package_directory qt_recipe_directory qt_runtime_directory
                CMAKE_CURRENT_SOURCE_DIR CMAKE_CURRENT_BINARY_DIR CMAKE_CXX_COMPILER
                CMAKE_OBJDUMP CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND Python3_EXECUTABLE
                qt_runtime_search_dirs qt_compiler_runtime_files qt_debug_runtime_files
                qt_notice_inputs qtbase_source runtime_notices_directory qt_extra_copies)
            string(REPLACE "\\" "/" ${path_variable} "${${path_variable}}")
        endforeach()
    endif()
    configure_file("${qt_runtime_directory}/qt_runtime_assemble.cmake.in"
        "${qt_recipe_directory}/assemble-package.cmake.in" @ONLY)
    file(GENERATE OUTPUT "${qt_recipe_directory}/assemble-package-$<CONFIG>.cmake"
        INPUT "${qt_recipe_directory}/assemble-package.cmake.in")
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(WIN32)
            set(check_template qt_runtime_windows_check.cmake.in)
        else()
            set(check_template qt_runtime_macos_check.cmake.in)
        endif()
        configure_file("${qt_runtime_directory}/${check_template}"
            "${qt_recipe_directory}/check-native-runtime-closure.cmake.in" @ONLY)
        file(GENERATE OUTPUT "${qt_recipe_directory}/check-native-runtime-closure-$<CONFIG>.cmake"
            INPUT "${qt_recipe_directory}/check-native-runtime-closure.cmake.in")
    endif()
    configure_file("${qt_runtime_directory}/qt_runtime_check.cmake.in"
        "${qt_recipe_directory}/check-runtime-closure.cmake.in" @ONLY)
    file(GENERATE OUTPUT "${qt_recipe_directory}/check-runtime-closure-$<CONFIG>.cmake"
        INPUT "${qt_recipe_directory}/check-runtime-closure.cmake.in")
    set(runtime_check "${CMAKE_COMMAND}" -P
        "${qt_recipe_directory}/check-runtime-closure-$<CONFIG>.cmake")

    # Runtime bytes can change without relinking any program. The assembler
    # checks content and membership while preserving complete no-op packages.
    add_custom_target(${target} ALL
        COMMAND "${CMAKE_COMMAND}" -P "${qt_recipe_directory}/assemble-package-$<CONFIG>.cmake"
        BYPRODUCTS "${qt_package_directory}/package.files.json"
        DEPENDS ${ARG_ADAPTER} ${ARG_HELPERS} logonomic_cli_addon_index
        VERBATIM)
    logonomic_cli_addon_install_package("${qt_package_directory}" "${package_id}" TARGET ${target})
    set(${out_directory} "${qt_package_directory}" PARENT_SCOPE)
    if(ARG_OUT_RUNTIME_CHECK)
        set(${ARG_OUT_RUNTIME_CHECK} "${runtime_check}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_QTBASE_SOURCE)
        set(${ARG_OUT_QTBASE_SOURCE} "${qtbase_source}" PARENT_SCOPE)
    endif()
endfunction()
