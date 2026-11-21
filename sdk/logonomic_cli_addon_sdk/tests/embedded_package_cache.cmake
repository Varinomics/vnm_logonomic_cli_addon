include_guard(GLOBAL)

function(logonomic_cli_addon_write_embedded_package_cache path)
    # A preload file preserves launcher argument lists across CTest, cmake -P
    # and the optional Windows environment wrapper without list expansion.
    if(NOT CMAKE_FASTBUILD_CACHE_PATH AND DEFINED ENV{FASTBUILD_CACHE_PATH})
        set(CMAKE_FASTBUILD_CACHE_PATH "$ENV{FASTBUILD_CACHE_PATH}")
    endif()
    set(content "# Generated compiler cache settings for the embedded package test.\n")
    foreach(variable IN ITEMS
        CMAKE_C_COMPILER_LAUNCHER
        CMAKE_CXX_COMPILER_LAUNCHER
        CMAKE_FASTBUILD_CACHE_PATH
        CMAKE_FASTBUILD_USE_LIGHTCACHE)
        if(NOT DEFINED ${variable})
            continue()
        endif()
        set(value "${${variable}}")
        set(equals "=")
        while(value MATCHES "]${equals}]")
            string(APPEND equals "=")
        endwhile()
        string(APPEND content
            "set(${variable} [${equals}[${value}]${equals}] CACHE STRING \"\" FORCE)\n")
    endforeach()
    file(WRITE "${path}" "${content}")
endfunction()
