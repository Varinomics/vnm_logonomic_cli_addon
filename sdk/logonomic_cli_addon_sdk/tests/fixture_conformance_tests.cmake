cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/check_conformance_package.cmake")

function(read_report directory expected)
    execute_process(COMMAND "${CONFORMANCE_TOOL}" --json "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE error)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "Expected conformance exit ${expected} for ${directory}: ${result}: ${report}${error}")
    endif()
    string(JSON version GET "${report}" report_version)
    string(JSON passed GET "${report}" qualification passed)
    string(JSON workflow GET "${report}" coverage desktop_workflows_exercised)
    if(NOT version EQUAL 1 OR workflow OR (expected EQUAL 0 AND NOT passed) OR (expected EQUAL 1 AND passed))
        message(FATAL_ERROR "Incorrect qualification or workflow coverage: ${report}")
    endif()
    set(report "${report}" PARENT_SCOPE)
endfunction()

# Positive fixtures must obey the same advertised-pair contract as packages.
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-usage-cache-acceptance"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT output MATCHES "PASS handshake.sidecars_live")
    message(FATAL_ERROR "Usage-cache acceptance fixture failed conformance: ${result}: ${output}${error}")
endif()
read_report("${FIXTURE_PACKAGES}/org.example.unknown-fixture" 0)
string(JSON valid GET "${report}" package valid)
string(JSON id GET "${report}" package id)
string(JSON required GET "${report}" qualification required_live)
string(JSON unobserved GET "${report}" coverage declared_without_observed_result)
if(NOT valid OR NOT id STREQUAL "org.example.unknown-fixture" OR NOT required MATCHES "profile-storage@1" OR
        NOT unobserved MATCHES "org.example.unadopted@7")
    message(FATAL_ERROR "Valid package, expected availability or unexercised pair missing: ${report}")
endif()
string(JSON count LENGTH "${report}" coverage adapter_operations_with_results)
if(count EQUAL 0)
    message(FATAL_ERROR "The positive probe fixture returned no observed operations: ${report}")
endif()
math(EXPR last "${count} - 1")
set(operations)
foreach(i RANGE ${last})
    string(JSON row GET "${report}" coverage adapter_operations_with_results ${i})
    string(JSON capability GET "${row}" capability)
    string(JSON operation GET "${row}" operation)
    string(JSON status GET "${row}" host_status)
    list(APPEND operations "${capability}/${operation}/${status}")
endforeach()
if(NOT "conformance-probe@1/echo/success" IN_LIST operations OR
        NOT "profile-storage@1/acknowledge/success" IN_LIST operations OR
        NOT "org.example.absent@1/noop/unavailable" IN_LIST operations OR
        "conformance-probe@7/echo/unsupported_version" IN_LIST operations OR
        "org.example.absent@1/noop/unknown_capability" IN_LIST operations)
    message(FATAL_ERROR "Adapter results and locally refused probes were misclassified: ${operations}")
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
read_report("${FIXTURE_PACKAGES}/org.example.unknown-fixture-sidecar-without-handshake" 1)
string(JSON valid GET "${report}" package valid)
string(JSON required GET "${report}" qualification required_live)
string(JSON live GET "${report}" availability live)
string(JSON unobserved GET "${report}" coverage declared_without_observed_result)
if(NOT valid OR NOT required MATCHES "profile-storage@1" OR live MATCHES "profile-storage@1" OR
        NOT unobserved MATCHES "profile-storage@1")
    message(FATAL_ERROR "Missing dynamic pair confused validity with current availability: ${report}")
endif()
execute_process(COMMAND "${CONFORMANCE_TOOL}" "${FIXTURE_PACKAGES}/org.example.unknown-fixture-handshake-without-sidecar"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT output MATCHES "FAIL handshake.no_undeclared_pairs.*org.example.extra@1")
    message(FATAL_ERROR "Undeclared-pair negative fixture did not fail as intended: ${result}: ${output}${error}")
endif()
message(STATUS "Positive and static metadata fixtures conform; missing dynamic and undeclared handshake pairs remain rejected")

set(launcher_fixture "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-surfaces")
read_report("${launcher_fixture}" 0)
string(JSON live GET "${report}" availability live)
string(JSON unobserved GET "${report}" coverage declared_without_observed_result)
string(JSON checks GET "${report}" checks)
if(NOT live MATCHES "launcher-discovery@1" OR NOT unobserved MATCHES "launcher-discovery@1" OR
        NOT checks MATCHES "\"SKIP\"")
    message(FATAL_ERROR "Launcher declaration or handshake was reported as exercised behavior: ${report}")
endif()
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
read_report("${directory}" 1)
string(JSON valid GET "${report}" package valid)
string(JSON availability TYPE "${report}" availability)
string(JSON count LENGTH "${report}" coverage adapter_operations_with_results)
if(NOT valid OR NOT availability STREQUAL "NULL" OR NOT count EQUAL 0)
    message(FATAL_ERROR "Failed declaration checks fabricated runtime observations: ${report}")
endif()
read_report("${TEST_ROOT}/missing-package" 1)
string(JSON valid GET "${report}" package valid)
string(JSON expected_type TYPE "${report}" qualification required_live)
if(valid OR NOT expected_type STREQUAL "NULL")
    message(FATAL_ERROR "Missing package reported a validated contract or handshake expectations: ${report}")
endif()

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
