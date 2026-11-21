cmake_minimum_required(VERSION 3.21)

foreach(required IN ITEMS CONFORMANCE_TOOL VALIDATE_TOOL INDEX_TOOL FIXTURE_PACKAGES TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/check_conformance_package.cmake")

function(session_case name version family payload expected)
    set(directory "${TEST_ROOT}/${name}")
    file(MAKE_DIRECTORY "${directory}")
    file(COPY "${FIXTURE_PACKAGES}/org.example.unknown-fixture-provider-sessions/"
        DESTINATION "${directory}")
    file(REMOVE "${directory}/capabilities/structured-session@3.json"
                "${directory}/capabilities/delegated-session@3.json")
    file(READ "${directory}/logonomic-cli-addon.json" bootstrap)
    string(JSON bootstrap SET "${bootstrap}" adapter argv
        "[\"--behavior\",\"provider-sessions\",\"--session-declaration-version\",\"${version}\"]")
    file(WRITE "${directory}/logonomic-cli-addon.json" "${bootstrap}")
    foreach(id IN ITEMS structured-session delegated-session)
        set(declaration "{\"purposes\":[\"read_only_review\"]}")
        if(version EQUAL 3)
            string(JSON declaration SET "${declaration}" environment_names "[]")
        endif()
        if(id STREQUAL family)
            set(declaration "${payload}")
        endif()
        file(WRITE "${directory}/capabilities/${id}@${version}.json"
            "{\"capability_id\":\"${id}\",\"version\":${version},\"payload\":${declaration}}")
    endforeach()
    if(expected EQUAL 1)
        set(check "FAIL declaration.session_purposes ${family}@${version}: invalid_sidecar:")
    elseif(version GREATER 3)
        set(check "SKIP declaration.unassessed ${family}@${version}: unsupported declaration version")
    else()
        set(check "PASS declaration.session_purposes ${family}@${version}")
    endif()
    check_package("${name}" "${directory}" "${expected}" "${check}")
endfunction()

# A correctly indexed malformed declaration used to pass the entire suite.
foreach(family IN ITEMS structured-session delegated-session)
    session_case("${family}-3-bad-purpose" 3 "${family}"
        "{\"purposes\":[\"read-only-review\"],\"environment_names\":[]}" 1)
    session_case("${family}-3-valid" 3 "${family}"
        "{\"purposes\":[\"read_only_review\"],\"environment_names\":[]}" 0)
    session_case("${family}-3-bad-environment" 3 "${family}"
        "{\"purposes\":[\"read_only_review\"],\"environment_names\":42}" 1)
    session_case("${family}-3-valid-environment" 3 "${family}"
        "{\"purposes\":[\"read_only_review\"],\"environment_names\":[\"FIXTURE_SESSION_INPUT\"]}" 0)
    session_case("${family}-4-opaque" 4 "${family}" "{\"future_contract\":42}" 0)
endforeach()

set(directory "${TEST_ROOT}/unknown-id-opaque")
file(MAKE_DIRECTORY "${directory}")
file(COPY "${FIXTURE_PACKAGES}/org.example.unknown-fixture-handshake-without-sidecar/"
    DESTINATION "${directory}")
file(WRITE "${directory}/capabilities/org.example.extra@1.json"
    "{\"capability_id\":\"org.example.extra\",\"version\":1,\"payload\":{\"future_contract\":42}}")
check_package(unknown-id-opaque "${directory}" 0
    "SKIP declaration.unassessed org.example.extra@1: no declaration decoder in this suite")
