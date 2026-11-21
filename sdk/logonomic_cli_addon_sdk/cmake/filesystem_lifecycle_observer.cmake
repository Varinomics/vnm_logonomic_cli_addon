# Providers select the maintained filesystem service before opting into this
# component. The ordinary SDK remains independent of Qt and that service.
function(logonomic_cli_addon_add_filesystem_observer)
    if(TARGET logonomic_cli_addon_sdk::filesystem_observer)
        return()
    endif()
    if(NOT TARGET vnm_framework::vnm_filesystem_observation)
        message(FATAL_ERROR "Select vnm_framework::vnm_filesystem_observation before adding the lifecycle observer")
    endif()
    get_target_property(observer_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_library(logonomic_cli_addon_filesystem_observer STATIC
        "${observer_sdk_source}/src/filesystem_lifecycle_observer.cpp")
    add_library(logonomic_cli_addon_sdk::filesystem_observer ALIAS logonomic_cli_addon_filesystem_observer)
    target_link_libraries(logonomic_cli_addon_filesystem_observer PUBLIC
        logonomic_cli_addon_sdk::sdk vnm_framework::vnm_filesystem_observation)
    logonomic_cli_addon_sdk_configure_target(logonomic_cli_addon_filesystem_observer)
endfunction()
