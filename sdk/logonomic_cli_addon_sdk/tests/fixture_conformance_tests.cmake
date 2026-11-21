# Positive fixtures must obey the same advertised-pair contract as packages.
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-usage-cache-acceptance"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "PASS handshake.sidecars_live")
    message(FATAL_ERROR "Usage-cache acceptance fixture failed conformance: ${result}: ${output}${error}")
endif()
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Positive fixture failed conformance: ${output}${error}")
endif()
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-account-managed-configuration"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "PASS handshake.sidecars_live")
    message(FATAL_ERROR "Static account-profile metadata fixture failed conformance: ${result}: ${output}${error}")
endif()
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-sidecar-without-handshake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT output MATCHES "FAIL handshake.sidecars_live[^\n]*profile-storage@1")
    message(FATAL_ERROR "Missing dynamic handshake pair did not fail as intended: ${result}: ${output}${error}")
endif()
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-handshake-without-sidecar"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT output MATCHES "FAIL handshake.no_undeclared_pairs.*org.example.extra@1")
    message(FATAL_ERROR "Undeclared-pair negative fixture did not fail as intended: ${result}: ${output}${error}")
endif()
message(STATUS "Positive and static metadata fixtures conform; missing dynamic and undeclared handshake pairs remain rejected")
