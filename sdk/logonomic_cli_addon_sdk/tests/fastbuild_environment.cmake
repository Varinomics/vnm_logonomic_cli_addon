cmake_minimum_required(VERSION 3.21)

# Check the generated SDK and any retained compiler probes. Explicit user-
# supplied compiler overrides are separate from ambient environment capture.
load_cache("${SDK_BUILD_DIRECTORY}" READ_WITH_PREFIX sdk_
    CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_ENV_OVERRIDES)
if(NOT DEFINED sdk_CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OR sdk_CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OR
   NOT "CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV" IN_LIST sdk_CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)
    message(FATAL_ERROR "Standalone FASTBuild must disable environment capture before compiler checks")
endif()
if(NOT sdk_CMAKE_FASTBUILD_ENV_OVERRIDES)
    file(GLOB_RECURSE graphs "${SDK_BUILD_DIRECTORY}/fbuild.bff")
    if(NOT graphs)
        message(FATAL_ERROR "Standalone SDK has no generated FASTBuild graph")
    endif()
    foreach(graph IN LISTS graphs)
        file(STRINGS "${graph}" captured LIMIT_COUNT 1 REGEX "^[ \t]*\\.Environment[ \t]*=[ \t]*\\.LocalEnv")
        if(captured)
            message(FATAL_ERROR "Standalone SDK graph uses the configured environment in ${graph}")
        endif()
    endforeach()
endif()
