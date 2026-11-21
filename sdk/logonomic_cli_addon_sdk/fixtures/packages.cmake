# The conformance fixture packages and the function that assembles them.
# Every package is built from the one fixture adapter binary; behaviour
# variants differ only in the bootstrap's adapter.argv, package-file variants
# ship a defective bootstrap, overlay a defective static file, or ask the index
# tool for a deliberate index defect. The provider-session packages add the
# session sidecars they share and the packaged helper binary under helpers/.
# docs/conformance.md lists what each package is designed to fail.
#
# The roster and the fixture source directory are resolved inside the
# function, from the directory of this file, because a CMake function reads
# the variables of its caller: a project that consumed the SDK through
# add_subdirectory and calls the function from its own directory scope must
# assemble the same packages as the SDK's own CMakeLists.txt does.

# logonomic_cli_addon_sdk_add_fixture_packages(OUTPUT_DIRECTORY <dir> TARGET <name> [PACKAGE_IDS <ids>...])
#
# Assembles every fixture package under <dir>/<package id>/ after the adapter,
# the helper and the index tool are built: the shared static files, the
# session packages' shared sidecars, the package's own files on top, the
# adapter binary under adapter/, the helper binary under helpers/ for the
# session packages, and package.files.json written by
# logonomic_cli_addon_index. <name> is a target that depends on all of them;
# callers add it as a dependency of whatever consumes the packages.
function(logonomic_cli_addon_sdk_add_fixture_packages)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "OUTPUT_DIRECTORY;TARGET" "PACKAGE_IDS")
    if(NOT ARG_OUTPUT_DIRECTORY OR NOT ARG_TARGET)
        message(FATAL_ERROR
            "logonomic_cli_addon_sdk_add_fixture_packages needs OUTPUT_DIRECTORY and TARGET")
    endif()

    set(_source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/packages")

    set(_behaviors
        crash-on-invoke
        hang-on-invoke
        hang-on-hello
        double-result
        oversize-frame
        invalid-utf8-frame
        stdout-garbage
        stderr-flood
        bidirectional-flood
        stdin-stall
        result-then-exit
        wrong-nonce-hello
        wrong-core-generation
        sidecar-without-handshake
        handshake-without-sidecar
        orphan-grandchild
        detach-grandchild
        ignores-cancel
        slow-shutdown
        slow-result
        profile-omit-echo
        profile-wrong-root
        profile-default-dir
        profile-wrong-version
        provider-surfaces
        provider-initial-prompt)

    # The provider-session packages: the two behaviours and the variants whose
    # adapter.argv adds --classify-hang or --resume-foreign-digest. They
    # overlay _sessions_shared and carry the helper.
    set(_session_packages
        org.example.unknown-fixture-provider-accounts
        org.example.unknown-fixture-provider-accounts-twin
        org.example.unknown-fixture-provider-sessions
        org.example.unknown-fixture-provider-sessions-context
        org.example.unknown-fixture-provider-sessions-permission-snapshot
        org.example.unknown-fixture-provider-sessions-assets
        org.example.unknown-fixture-provider-sessions-continuous
        org.example.unknown-fixture-provider-sessions-lifetime
        org.example.unknown-fixture-provider-sessions-stdin-eof
        org.example.unknown-fixture-provider-sessions-classify-hang
        org.example.unknown-fixture-provider-sessions-foreign-digest)

    # Independent auth/quota/usage capability subsets; no session sidecars.
    set(_observation_packages
        org.example.unknown-fixture-provider-auth
        org.example.unknown-fixture-provider-auth-accounts
        org.example.unknown-fixture-provider-quota
        org.example.unknown-fixture-provider-quota-incremental
        org.example.unknown-fixture-provider-quota-accounts
        org.example.unknown-fixture-provider-usage
        org.example.unknown-fixture-provider-usage-accounts
        org.example.unknown-fixture-provider-auth-quota
        org.example.unknown-fixture-provider-auth-quota-accounts
        org.example.unknown-fixture-provider-current-account
        org.example.unknown-fixture-provider-account-refresh
        org.example.unknown-fixture-provider-account-refresh-unknown
        org.example.unknown-fixture-provider-auth-usage
        org.example.unknown-fixture-provider-auth-usage-accounts
        org.example.unknown-fixture-provider-quota-usage
        org.example.unknown-fixture-provider-quota-usage-accounts
        org.example.unknown-fixture-provider-observations
        org.example.unknown-fixture-provider-observations-accounts
        org.example.unknown-fixture-provider-usage-cache-acceptance
        org.example.unknown-fixture-provider-observations-auth-error
        org.example.unknown-fixture-provider-observations-quota-error
        org.example.unknown-fixture-provider-observations-process-error
        org.example.unknown-fixture-provider-observations-timeout
        org.example.unknown-fixture-provider-observations-wrong-account
        org.example.unknown-fixture-provider-observations-expiring
        org.example.unknown-fixture-provider-observations-stale
        org.example.unknown-fixture-provider-observations-not-logged-in
        org.example.unknown-fixture-provider-observations-usage-duplicate
        org.example.unknown-fixture-provider-observations-usage-partial
        org.example.unknown-fixture-provider-observations-usage-empty-page
        org.example.unknown-fixture-provider-observations-usage-observation-change
        org.example.unknown-fixture-provider-observations-usage-snapshot-change
        org.example.unknown-fixture-provider-observations-usage-cursor-repeat
        org.example.unknown-fixture-provider-observations-usage-incomplete-history
        org.example.unknown-fixture-provider-observations-usage-cancel
        org.example.unknown-fixture-provider-observations-unknown-auth
        org.example.unknown-fixture-provider-observations-unknown-quota
        org.example.unknown-fixture-provider-observations-unknown-usage)

    set(_managed_configuration_packages
        org.example.unknown-fixture-provider-account-managed-configuration
        org.example.unknown-fixture-provider-managed-configuration-malformed-v2
        org.example.unknown-fixture-provider-managed-configuration-nonlive-v2
        org.example.unknown-fixture-provider-managed-configuration-v2)

    # Each entry is `<package id>|<index defect or empty>`.
    set(_packages "org.example.unknown-fixture|")
    foreach(_behavior IN LISTS _behaviors)
        list(APPEND _packages "org.example.unknown-fixture-${_behavior}|")
    endforeach()
    foreach(_package_id IN LISTS _session_packages _observation_packages _managed_configuration_packages)
        list(APPEND _packages "${_package_id}|")
    endforeach()
    list(APPEND _packages
        "org.example.unknown-fixture-invalid-bootstrap|"
        "org.example.unknown-fixture-reserved-environment|"
        "org.example.unknown-fixture-escaped-path|escaped-path"
        "org.example.unknown-fixture-invalid-utf8-bootstrap|"
        "org.example.unknown-fixture-digest-mismatch|digest-mismatch"
        "org.example.unknown-fixture-unsupported-core-generation|"
        "org.example.unknown-fixture-third-party-ok|"
        "org.example.unknown-fixture-third-party-missing-notice|third-party-missing-notice"
        "org.example.unknown-fixture-third-party-empty-notice|third-party-empty-notice"
        "org.example.unknown-fixture-third-party-escaped-notice|third-party-escaped-notice"
        "org.example.unknown-fixture-third-party-unindexed-notice|third-party-unindexed-notice"
        "org.example.unknown-fixture-third-party-digest-mismatch|third-party-digest-mismatch")

    file(GLOB_RECURSE _static_files CONFIGURE_DEPENDS "${_source_dir}/*")

    set(_remaining_ids ${ARG_PACKAGE_IDS})
    set(_outputs)
    foreach(_entry IN LISTS _packages)
        string(FIND "${_entry}" "|" _separator)
        string(SUBSTRING "${_entry}" 0 ${_separator} _package_id)
        if(ARG_PACKAGE_IDS AND NOT _package_id IN_LIST ARG_PACKAGE_IDS)
            continue()
        endif()
        list(REMOVE_ITEM _remaining_ids "${_package_id}")
        math(EXPR _defect_start "${_separator} + 1")
        string(SUBSTRING "${_entry}" ${_defect_start} -1 _defect)

        set(_destination "${ARG_OUTPUT_DIRECTORY}/${_package_id}")
        set(_native_commands)
        # Invalid bootstraps keep their exact source bytes and defects.
        if(APPLE AND
            NOT _package_id STREQUAL "org.example.unknown-fixture-invalid-bootstrap" AND
            NOT _package_id STREQUAL "org.example.unknown-fixture-invalid-utf8-bootstrap")
            set(_native_commands
                COMMAND "${CMAKE_COMMAND}"
                    "-DPACKAGE_DIRECTORY=${_destination}"
                    "-DADAPTER_NAME=$<TARGET_FILE_NAME:logonomic_cli_addon_unknown_fixture_adapter>"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/macos_package.cmake")
        endif()
        set(_defect_arguments)
        if(_defect)
            set(_defect_arguments --inject-defect "${_defect}")
        endif()

        # The session overlay goes between the shared files and the package's
        # own, so a package still wins over the sidecars it shares.
        set(_session_commands)
        set(_session_dependencies)
        if(_package_id IN_LIST _session_packages)
            set(_session_commands
                COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_source_dir}/_sessions_shared" "${_destination}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_destination}/helpers"
                COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:logonomic_cli_addon_unknown_fixture_helper>" "${_destination}/helpers/")
            set(_session_dependencies logonomic_cli_addon_unknown_fixture_helper)
        endif()

        if(_package_id IN_LIST _observation_packages)
            set(_session_commands
                COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_source_dir}/_observations_shared" "${_destination}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_destination}/helpers"
                COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:logonomic_cli_addon_unknown_fixture_helper>" "${_destination}/helpers/")
            set(_session_dependencies logonomic_cli_addon_unknown_fixture_helper)
        endif()

        add_custom_command(
            OUTPUT "${_destination}/package.files.json"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_destination}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_source_dir}/_shared" "${_destination}"
            ${_session_commands}
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_source_dir}/${_package_id}" "${_destination}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_destination}/adapter"
            COMMAND "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:logonomic_cli_addon_unknown_fixture_adapter>" "${_destination}/adapter/"
            ${_native_commands}
            COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${_destination}"
                --third-party assets/mark.svg notices/mark-license.txt provenance/mark.json
                ${_defect_arguments}
            DEPENDS
                logonomic_cli_addon_index
                logonomic_cli_addon_unknown_fixture_adapter
                ${_session_dependencies}
                ${_static_files}
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/macos_package.cmake"
            COMMENT "Assembling fixture package ${_package_id}"
            VERBATIM)
        list(APPEND _outputs "${_destination}/package.files.json")
    endforeach()

    if(_remaining_ids)
        message(FATAL_ERROR "Unknown add-on fixture package IDs: ${_remaining_ids}")
    endif()
    add_custom_target(${ARG_TARGET} DEPENDS ${_outputs})
endfunction()
