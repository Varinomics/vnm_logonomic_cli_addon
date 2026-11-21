cmake_minimum_required(VERSION 3.21)

# Oracle: docs/package_contract.md, "Embedded build". Embedding changes how a
# package is staged, never the package. This builds the package project
# PACKAGE_SOURCE twice from the same sources: on its own, the way its
# repository builds it, and as the subproject of tests/embedded_host. Then:
#
# - the standalone install exports exactly one package, byte-identical to the
#   package the build assembled;
# - the embedded package registers exactly once, in its own project's binary
#   directory, naming a target that assembles it when built alone;
# - the embedded package changes no cache entry the embedding build holds,
#   adds none named CMAKE_*, LOGONOMIC_*, BUILD_SHARED_LIBS or BUILD_TESTING,
#   leaves the build properties of the directory that added it as they were,
#   and enables no testing there (global properties, which CMake cannot list,
#   are not compared);
# - the embedding build installs nothing;
# - both packages validate with one generation digest, which equals
#   EXPECTED_GENERATION_DIGEST when that is given.
#
# The digests compare because both builds are reproducible: Release, compiled
# locally and, with MSVC, linked with /Brepro (reproducible_link.cmake). A
# distributed compile is not: MSVC mangles an anonymous-namespace name from the
# path of the file it compiles, and two distributed FASTBuild builds of the
# same sources mangle it differently. For the same reason, digests from
# separate runs compare only when the package and the SDK sources are at the
# same paths in both runs.
#
# STANDALONE_ONLY=ON builds only the standalone half, to record the digest of
# a package revision that cannot be embedded yet; a later run compares against
# it through EXPECTED_GENERATION_DIGEST.
#
# The run compiles, and acquires no CPUs itself: its configures probe the
# compiler and its builds, one after the other, each run TEST_PARALLEL_LEVEL
# compile jobs. The caller grants that width, as one build-queue request for
# TEST_PARALLEL_LEVEL slots where builds are queued.
#
# Required: PACKAGE_SOURCE, VALIDATE_TOOL (logonomic_cli_addon_validate built
# from this SDK), TEST_ROOT (recreated) and TEST_GENERATOR. Optional:
# TEST_MAKE_PROGRAM, TEST_CXX_COMPILER, TEST_TOOLCHAIN_FILE,
# TEST_COMPILE_CACHE_FILE (CMake preload outside TEST_ROOT),
# TEST_FASTBUILD_USE_CACHE (enable the configured FASTBuild cache),
# TEST_PARALLEL_LEVEL (compile jobs in each build, default 1),
# EXPECTED_GENERATION_DIGEST, STANDALONE_ONLY, and on Windows TEST_VCVARSALL
# with TEST_VCVARS_VERSION to enter the MSVC developer environment first.

foreach(required IN ITEMS PACKAGE_SOURCE VALIDATE_TOOL TEST_ROOT TEST_GENERATOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT DEFINED TEST_PARALLEL_LEVEL)
    set(TEST_PARALLEL_LEVEL 1)
endif()
if(NOT TEST_PARALLEL_LEVEL MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "TEST_PARALLEL_LEVEL must be a positive job count: '${TEST_PARALLEL_LEVEL}'")
endif()

# An absolute cl.exe selects the compiler but not its INCLUDE and LIB, and
# CTest need not run inside the developer environment, so the whole run is
# repeated once inside the environment of the named toolset.
if(DEFINED TEST_VCVARSALL)
    if(NOT IS_ABSOLUTE "${TEST_VCVARSALL}" OR NOT EXISTS "${TEST_VCVARSALL}" OR
       IS_DIRECTORY "${TEST_VCVARSALL}")
        message(FATAL_ERROR "TEST_VCVARSALL must name vcvarsall.bat: '${TEST_VCVARSALL}'")
    endif()
    if(NOT DEFINED TEST_VCVARS_VERSION OR NOT TEST_VCVARS_VERSION MATCHES "^[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "TEST_VCVARS_VERSION must be a major.minor toolset version: '${TEST_VCVARS_VERSION}'")
    endif()
    set(inner_command "${CMAKE_COMMAND}")
    foreach(input IN ITEMS
        PACKAGE_SOURCE
        VALIDATE_TOOL
        TEST_ROOT
        TEST_GENERATOR
        TEST_MAKE_PROGRAM
        TEST_CXX_COMPILER
        TEST_TOOLCHAIN_FILE
        TEST_COMPILE_CACHE_FILE
        TEST_FASTBUILD_USE_CACHE
        TEST_PARALLEL_LEVEL
        EXPECTED_GENERATION_DIGEST
        STANDALONE_ONLY)
        if(DEFINED ${input})
            list(APPEND inner_command "-D${input}=${${input}}")
        endif()
    endforeach()
    list(APPEND inner_command -P "${CMAKE_CURRENT_LIST_FILE}")
    string(CONCAT batch_contents
        "@echo off\r\n"
        "call \"${TEST_VCVARSALL}\" x64 -vcvars_ver=${TEST_VCVARS_VERSION} >nul\r\n"
        "if errorlevel 1 exit /b %errorlevel%\r\n")
    foreach(argument IN LISTS inner_command)
        if(argument MATCHES "[\"%]")
            message(FATAL_ERROR
                "The argument cannot be passed through cmd.exe unchanged: '${argument}'")
        endif()
        string(APPEND batch_contents " \"${argument}\"")
    endforeach()
    string(APPEND batch_contents "\r\nexit /b %errorlevel%\r\n")
    # Beside TEST_ROOT, not in it: the inner run recreates TEST_ROOT while
    # cmd.exe is still reading this file.
    set(batch "${TEST_ROOT}-environment.bat")
    file(WRITE "${batch}" "${batch_contents}")
    execute_process(COMMAND "${batch}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "The embedded package comparison failed in the MSVC developer environment (${result}).")
    endif()
    return()
endif()

get_filename_component(sdk_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(host_source "${CMAKE_CURRENT_LIST_DIR}/embedded_host")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")

function(embedded_package_run step)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${step} failed (${result}): ${ARGN}\n${output}\n${error}")
    endif()
endfunction()

function(embedded_package_digest directory package_id out_digest)
    execute_process(COMMAND "${VALIDATE_TOOL}" "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0 OR NOT output MATCHES "^OK ([^ ]+) ([0-9a-f]+) bytes=")
        message(FATAL_ERROR "${directory} is not a valid package: ${output}\n${error}")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL package_id)
        message(FATAL_ERROR "${directory} holds package ${CMAKE_MATCH_1}, not ${package_id}")
    endif()
    set(${out_digest} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Names every file whose bytes differ between two package directories, so a
# digest mismatch says what the builds disagree about.
function(embedded_package_differences first second out_differences)
    set(differences)
    file(GLOB_RECURSE first_files LIST_DIRECTORIES FALSE RELATIVE "${first}" "${first}/*")
    file(GLOB_RECURSE second_files LIST_DIRECTORIES FALSE RELATIVE "${second}" "${second}/*")
    set(files ${first_files} ${second_files})
    list(REMOVE_DUPLICATES files)
    list(SORT files)
    foreach(file IN LISTS files)
        if(NOT EXISTS "${first}/${file}" OR NOT EXISTS "${second}/${file}")
            list(APPEND differences "${file} (present in one package only)")
            continue()
        endif()
        file(SHA256 "${first}/${file}" first_hash)
        file(SHA256 "${second}/${file}" second_hash)
        if(NOT first_hash STREQUAL second_hash)
            list(APPEND differences "${file}")
        endif()
    endforeach()
    list(JOIN differences "\n  " differences)
    set(${out_differences} "${differences}" PARENT_SCOPE)
endfunction()

# The package directory's configuration component, as built and as registered.
if(TEST_GENERATOR MATCHES "^(Visual Studio|Xcode|Ninja Multi-Config)")
    set(configuration_directory "Release/")
    set(registered_configuration_directory "$<CONFIG>/")
else()
    set(configuration_directory "")
    set(registered_configuration_directory "")
endif()

set(configure_arguments
    -G "${TEST_GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/reproducible_link.cmake")
if(DEFINED TEST_COMPILE_CACHE_FILE AND NOT TEST_COMPILE_CACHE_FILE STREQUAL "")
    list(APPEND configure_arguments -C "${TEST_COMPILE_CACHE_FILE}")
endif()
if(TEST_GENERATOR STREQUAL "FASTBuild")
    list(APPEND configure_arguments
        -DCMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV=OFF
        -DCMAKE_TRY_COMPILE_PLATFORM_VARIABLES=CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(DEFINED TEST_MAKE_PROGRAM AND NOT TEST_MAKE_PROGRAM STREQUAL "")
    list(APPEND configure_arguments "-DCMAKE_MAKE_PROGRAM=${TEST_MAKE_PROGRAM}")
endif()
if(DEFINED TEST_CXX_COMPILER AND NOT TEST_CXX_COMPILER STREQUAL "")
    list(APPEND configure_arguments "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}")
endif()
if(DEFINED TEST_TOOLCHAIN_FILE AND NOT TEST_TOOLCHAIN_FILE STREQUAL "")
    list(APPEND configure_arguments "-DCMAKE_TOOLCHAIN_FILE=${TEST_TOOLCHAIN_FILE}")
endif()

# A FASTBuild tree takes its job count natively; no build here distributes.
if(TEST_GENERATOR STREQUAL "FASTBuild")
    set(build_arguments --config Release -- -j${TEST_PARALLEL_LEVEL})
    if(TEST_FASTBUILD_USE_CACHE)
        list(APPEND build_arguments -cache)
    endif()
else()
    set(build_arguments --config Release --parallel ${TEST_PARALLEL_LEVEL})
endif()

set(standalone_build "${TEST_ROOT}/standalone")
set(standalone_prefix "${TEST_ROOT}/standalone-install")
embedded_package_run("The standalone configure"
    "${CMAKE_COMMAND}" -S "${PACKAGE_SOURCE}" -B "${standalone_build}" ${configure_arguments}
    "-DLOGONOMIC_CLI_ADDON_SDK_SOURCE=${sdk_source}")
embedded_package_run("The standalone build"
    "${CMAKE_COMMAND}" --build "${standalone_build}" ${build_arguments})
embedded_package_run("The standalone install"
    "${CMAKE_COMMAND}" -E env --unset=DESTDIR
    "${CMAKE_COMMAND}" --install "${standalone_build}"
        --config Release --prefix "${standalone_prefix}")

file(GLOB exported LIST_DIRECTORIES TRUE RELATIVE "${standalone_prefix}" "${standalone_prefix}/*")
list(LENGTH exported exported_count)
if(NOT exported_count EQUAL 1)
    message(FATAL_ERROR
        "The standalone install must export exactly one package; it exported '${exported}'")
endif()
set(package_id "${exported}")
set(standalone_package "${standalone_build}/package/${configuration_directory}${package_id}")
embedded_package_digest("${standalone_package}" "${package_id}" standalone_digest)
embedded_package_digest("${standalone_prefix}/${package_id}" "${package_id}" exported_digest)
if(NOT exported_digest STREQUAL standalone_digest)
    message(FATAL_ERROR "The standalone export is not the package the build assembled: "
        "${exported_digest} against ${standalone_digest}")
endif()
message(STATUS "standalone ${package_id} ${standalone_digest}")
if(DEFINED EXPECTED_GENERATION_DIGEST AND NOT standalone_digest STREQUAL EXPECTED_GENERATION_DIGEST)
    message(FATAL_ERROR "The standalone package ${package_id} has generation ${standalone_digest}, "
        "not the expected ${EXPECTED_GENERATION_DIGEST}")
endif()
if(STANDALONE_ONLY)
    return()
endif()

set(host_build "${TEST_ROOT}/host")
set(host_prefix "${TEST_ROOT}/host-install")
embedded_package_run("The embedded configure"
    "${CMAKE_COMMAND}" -S "${host_source}" -B "${host_build}" ${configure_arguments}
    "-DPACKAGE_SOURCE=${PACKAGE_SOURCE}")

file(READ "${host_build}/embedded-packages.txt" registrations)
list(LENGTH registrations registration_count)
if(NOT registration_count EQUAL 1)
    message(FATAL_ERROR
        "The embedded package must register exactly once; it registered '${registrations}'")
endif()
# The ID and the target name hold no '|', so the directory is everything
# between the first and the last one.
string(FIND "${registrations}" "|" first_separator)
string(FIND "${registrations}" "|" last_separator REVERSE)
math(EXPR directory_start "${first_separator} + 1")
math(EXPR directory_length "${last_separator} - ${directory_start}")
math(EXPR target_start "${last_separator} + 1")
string(SUBSTRING "${registrations}" 0 ${first_separator} registered_id)
string(SUBSTRING "${registrations}" ${directory_start} ${directory_length} registered_directory)
string(SUBSTRING "${registrations}" ${target_start} -1 registered_target)
if(NOT registered_id STREQUAL package_id)
    message(FATAL_ERROR "The embedded package registered as ${registered_id}, not ${package_id}")
endif()
# The host project's binary directory as its project() recorded it, which is
# how the package's own project binary directory below it is spelled too.
file(STRINGS "${host_build}/CMakeCache.txt" host_binary_directory
    REGEX "^logonomic_cli_addon_embedded_host_BINARY_DIR:STATIC=")
string(REGEX REPLACE "^[^=]+=" "" host_binary_directory "${host_binary_directory}")
set(expected_directory
    "${host_binary_directory}/embedded/package/${registered_configuration_directory}${package_id}")
if(NOT registered_directory STREQUAL expected_directory)
    message(FATAL_ERROR "The embedded package registered the directory ${registered_directory}; "
        "its own project's package directory is ${expected_directory}")
endif()

# The cache entries adding the package created, changed or removed, as the
# host recorded them. Entries of the package's own and of the third-party
# projects it fetches are its business. A global write is any change to an
# entry the embedding build held, any new entry in the CMAKE_ or LOGONOMIC_
# names that build owns, and a new BUILD_SHARED_LIBS or BUILD_TESTING, the
# build-wide switches CMake reads without a prefix.
file(STRINGS "${host_build}/embedded-cache-changes.txt" cache_changes)
set(cache_writes)
set(cache_additions)
foreach(change IN LISTS cache_changes)
    if(change MATCHES "^added (.+)$")
        set(name "${CMAKE_MATCH_1}")
        if(name MATCHES "^((CMAKE|LOGONOMIC)_.*|BUILD_SHARED_LIBS|BUILD_TESTING)$")
            list(APPEND cache_writes "${change}")
        elseif(NOT name MATCHES "_(BINARY_DIR|SOURCE_DIR|IS_TOP_LEVEL)$")
            list(APPEND cache_additions "${name}")
        endif()
    else()
        list(APPEND cache_writes "${change}")
    endif()
endforeach()
if(cache_writes)
    list(JOIN cache_writes "\n  " cache_writes)
    message(FATAL_ERROR
        "The embedded package wrote to the embedding build's cache:\n  ${cache_writes}")
endif()
if(cache_additions)
    list(JOIN cache_additions " " cache_additions)
    message(STATUS "The embedded package added the cache entries ${cache_additions}")
endif()
# The build properties of the directory that added the package, which every
# host target added after it inherits.
file(STRINGS "${host_build}/embedded-directory-changes.txt" directory_changes)
if(directory_changes)
    list(JOIN directory_changes "\n  " directory_changes)
    message(FATAL_ERROR
        "The embedded package changed the build properties of the directory that added it:\n"
        "  ${directory_changes}")
endif()
# The host enables no testing, so a test file anywhere below it is the
# package's.
file(GLOB_RECURSE test_files "${host_build}/CTestTestfile.cmake")
if(test_files)
    list(JOIN test_files "\n  " test_files)
    message(FATAL_ERROR
        "The embedded package enabled testing in the embedding build:\n  ${test_files}")
endif()

# The registered target alone must assemble the package: an embedding build
# depends on nothing else to stage it.
embedded_package_run("The embedded build of ${registered_target}"
    "${CMAKE_COMMAND}" --build "${host_build}" --target "${registered_target}" ${build_arguments})
embedded_package_run("The embedded install"
    "${CMAKE_COMMAND}" -E env --unset=DESTDIR
    "${CMAKE_COMMAND}" --install "${host_build}" --config Release --prefix "${host_prefix}")
file(GLOB_RECURSE installed LIST_DIRECTORIES TRUE "${host_prefix}/*")
if(installed)
    message(FATAL_ERROR "The embedding build installed files: ${installed}")
endif()

file(READ "${host_build}/embedded-packages-Release.txt" resolved_registration)
string(SUBSTRING "${resolved_registration}" ${directory_start} -1 embedded_package)
string(FIND "${embedded_package}" "|" resolved_separator REVERSE)
string(SUBSTRING "${embedded_package}" 0 ${resolved_separator} embedded_package)
embedded_package_digest("${embedded_package}" "${package_id}" embedded_digest)
message(STATUS "embedded ${package_id} ${embedded_digest}")
if(NOT embedded_digest STREQUAL standalone_digest)
    embedded_package_differences("${standalone_package}" "${embedded_package}" differences)
    message(FATAL_ERROR "Embedding changed package ${package_id}: generation ${embedded_digest} "
        "embedded, ${standalone_digest} on its own. Files that differ:\n  ${differences}")
endif()
message(STATUS "Package ${package_id} is the same generation on its own and embedded")
