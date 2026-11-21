# Exercise the real CLI; a coercing parser must not turn bad evidence into a pass.
set(package "${FIXTURE_PACKAGES}/org.example.unknown-fixture")
function(expect_validation_exit expected)
    execute_process(COMMAND "${VALIDATE_TOOL}" "${package}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "validator ${ARGN}: expected ${expected}, got ${result}: ${output}${error}")
    endif()
endfunction()
expect_validation_exit(0)
expect_validation_exit(0 --core-generation 1)
expect_validation_exit(1 --core-generation 0)
expect_validation_exit(1 --core-generation 65535)
expect_validation_exit(0 --core-generation 65535 --core-generation 1)
foreach(value IN ITEMS "65537" "65536" "-65535" "-1" "1garbage" "1.0" "+1" " 1" "1 "
        "999999999999999999999999999999999999999")
    expect_validation_exit(2 --core-generation "${value}")
endforeach()
# Preserve the empty argument rather than expanding it through ARGN.
execute_process(COMMAND "${VALIDATE_TOOL}" "${package}" --core-generation ""
    RESULT_VARIABLE result)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "An empty core generation must be a usage error")
endif()
expect_validation_exit(2 --core-generation)
expect_validation_exit(2 --core-generation 1 --core-generation 1garbage)
message(STATUS "Validator CLI rejects malformed and out-of-range core generations")

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(target "${CMAKE_CURRENT_BINARY_DIR}/validator-desktop-${run_id}.json")
set(compatibility [=[{
    "compatibility_declaration_version":1,
    "bootstrap_versions":[1],"index_versions":[1],"core_protocol_generations":[1,2],
    "adopted_capabilities":[{"capability_id":"profile-storage","version":1},
        {"capability_id":"accounts","version":2},
        {"capability_id":"managed-configuration","version":2},
        {"capability_id":"account-profile-managed-configuration","version":1},
        {"capability_id":"interactive-initial-prompt","version":1},
        {"capability_id":"coordination-delivery","version":1}]
}]=])
file(WRITE "${target}" "${compatibility}")

function(inspect suffix expected)
    execute_process(COMMAND "${VALIDATE_TOOL}" "${package}${suffix}" --desktop "${target}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL expected)
        message(FATAL_ERROR "inspection of ${suffix} ${ARGN}: expected ${expected}, got ${status}: ${output}${error}")
    endif()
    set(report "${output}" PARENT_SCOPE)
endfunction()

function(expect_field expected)
    string(JSON value GET "${report}" ${ARGN})
    if(NOT "${value}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected ${ARGN} = ${expected}, got ${value} in ${report}")
    endif()
endfunction()

function(expect_pair id version declared adopted advertised)
    string(JSON count LENGTH "${report}" capabilities)
    math(EXPR last "${count}-1")
    foreach(index RANGE 0 ${last})
        string(JSON item GET "${report}" capabilities ${index})
        string(JSON pair_id GET "${item}" capability_id)
        string(JSON pair_version GET "${item}" version)
        if(pair_id STREQUAL id AND pair_version EQUAL version)
            expect_field("${declared}" capabilities ${index} declared)
            expect_field("${adopted}" capabilities ${index} adopted)
            expect_field("${advertised}" capabilities ${index} advertised)
            set(row ${index} PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Missing ${id}@${version} in ${report}")
endfunction()

# The wrong-nonce adapter fails when run. Offline inspection must still succeed.
inspect("-wrong-nonce-hello" 0)
expect_field("not_requested" activation)
expect_field("ON" package_valid)
expect_field("OFF" feature_behavior_tested)
expect_pair("profile-storage" 1 "ON" "ON" "")
inspect("-wrong-nonce-hello" 1 --live)
expect_field("failed" activation)

inspect("-sidecar-without-handshake" 0 --live)
expect_field("handshake_completed" activation)
expect_field("1" negotiated_core_generation)
expect_pair("profile-storage" 1 "ON" "ON" "OFF")
expect_pair("conformance-probe" 1 "ON" "OFF" "ON")
inspect("-handshake-without-sidecar" 0 --live)
expect_pair("org.example.extra" 1 "OFF" "OFF" "ON")

inspect("-provider-account-managed-configuration" 0 --live)
expect_pair("account-profile-managed-configuration" 1 "ON" "ON" "OFF")
expect_field("static_account_profile_binding" capabilities ${row} role)
string(JSON issue GET "${report}" capabilities ${row} contract_issue)
if(NOT issue MATCHES "managed-configuration@1")
    message(FATAL_ERROR "Missing the sidecar's exact managed-configuration prerequisite: ${issue}")
endif()
inspect("-provider-initial-prompt" 0)
expect_pair("interactive-initial-prompt" 1 "ON" "ON" "")
string(JSON issue GET "${report}" capabilities ${row} contract_issue)
if(NOT issue MATCHES "launch-preparation@3")
    message(FATAL_ERROR "Missing the interactive-initial-prompt prerequisite: ${issue}")
endif()
inspect("-provider-sessions" 0)
expect_pair("coordination-delivery" 1 "ON" "ON" "")
expect_field("static_coordination_delivery" capabilities ${row} role)

string(JSON incompatible SET "${compatibility}" bootstrap_versions "[2]")
string(JSON incompatible SET "${incompatible}" core_protocol_generations "[]")
file(WRITE "${target}" "${incompatible}")
inspect("" 1 --live)
expect_field("ON" package_valid)
expect_field("OFF" target_supports bootstrap)
expect_field("ON" target_supports index)
expect_field("OFF" target_supports core_protocol)
expect_field("not_started" activation)
string(JSON unknown SET "${compatibility}" compatibility_declaration_version "2")
file(WRITE "${target}" "${unknown}")
inspect("" 1)
expect_field("Expected compatibility_declaration_version 1." error)
file(REMOVE "${target}")
