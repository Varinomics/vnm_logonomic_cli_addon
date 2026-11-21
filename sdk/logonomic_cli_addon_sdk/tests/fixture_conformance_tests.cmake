cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/check_conformance_package.cmake")

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

set(launcher_fixture "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-surfaces")
set(discovery_ok "PASS declaration.launcher_environment launcher-discovery@1")
set(preparation_ok "PASS declaration.launcher_environment launch-preparation@3")
set(presentation_ok "PASS declaration.presentation presentation@1")

set(directory "${TEST_ROOT}/valid-launcher")
file(COPY "${launcher_fixture}/" DESTINATION "${directory}")
check_package(valid-launcher "${directory}" 0 "${discovery_ok}" "${preparation_ok}" "${presentation_ok}")

set(directory "${TEST_ROOT}/bad-environment")
file(COPY "${launcher_fixture}/" DESTINATION "${directory}")
file(WRITE "${directory}/capabilities/launcher-discovery@1.json" [=[
    {"capability_id":"launcher-discovery","version":1,"payload":{"environment_names":"PATH"}}
]=])
file(WRITE "${directory}/capabilities/launch-preparation@3.json" [=[
    {"capability_id":"launch-preparation","version":3,"payload":{"environment_names":["LOGONOMIC_PRIVATE"]}}
]=])
file(WRITE "${directory}/capabilities/presentation@2.json" [=[
    {"capability_id":"presentation","version":2,"payload":{"future_field":true}}
]=])
check_package(bad-environment "${directory}" 1 "${presentation_ok}"
    "FAIL declaration.launcher_environment launcher-discovery@1: invalid_sidecar"
    "FAIL declaration.launcher_environment launch-preparation@3: reserved_environment_variable"
    "SKIP declaration.unassessed presentation@2")

set(directory "${TEST_ROOT}/bad-presentation")
file(COPY "${launcher_fixture}/" DESTINATION "${directory}")
set(sidecar "capabilities/presentation@1.json")
file(READ "${launcher_fixture}/${sidecar}" presentation)
string(JSON invalid SET "${presentation}" payload display_name "\"\"")
file(WRITE "${directory}/${sidecar}" "${invalid}")
check_package(bad-display-name "${directory}" 1 "${discovery_ok}" "${preparation_ok}"
    "FAIL declaration.presentation presentation@1: presentation_invalid")

string(JSON invalid SET "${presentation}" payload icon dark "\"assets/missing.svg\"")
file(WRITE "${directory}/${sidecar}" "${invalid}")
check_package(missing-icon "${directory}" 1 "${discovery_ok}" "${preparation_ok}"
    "FAIL declaration.presentation presentation@1: presentation_invalid: assets/missing.svg")

string(JSON invalid SET "${presentation}" payload icon dark "\"assets/dark.svg\"")
file(WRITE "${directory}/${sidecar}" "${invalid}")
file(WRITE "${directory}/assets/dark.svg" [=[
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 16"><path d="M0 0h16v16z"/></svg>
]=])
check_package(bad-icon "${directory}" 1 "${discovery_ok}" "${preparation_ok}"
    "FAIL declaration.presentation presentation@1: icon_dimensions_invalid: assets/dark.svg")
