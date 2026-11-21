cmake_minimum_required(VERSION 3.21)
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(script "${TEST_ROOT}/deadline.json")
set(package "${FIXTURE_PACKAGES}/org.example.unknown-fixture")

function(check_deadline member expected)
    file(WRITE "${script}" [=[{"script_version":1,"steps":[
        {"op":"invoke","id":"echo","capability_id":"conformance-probe","version":1,"operation":"echo"]=]
        "${member}" [=[},
        {"op":"wait_result","id":"echo"},{"op":"shutdown"}]}]=])
    execute_process(COMMAND "${SIMULATOR_TOOL}" "${package}" --script "${script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE transcript ERROR_VARIABLE error TIMEOUT 10)
    if(expected STREQUAL "invalid")
        if(NOT result STREQUAL "1" OR NOT error MATCHES "script error:.*deadline_ms" OR
                transcript MATCHES "\"type\":\"invoke\"")
            message(FATAL_ERROR "Expected a script error before invoke for ${member}: ${result} ${error}\n${transcript}")
        endif()
    elseif(NOT result STREQUAL "0" OR NOT transcript MATCHES "\"deadline_ms\":${expected}[,}]")
        message(FATAL_ERROR "Expected wire deadline ${expected} for ${member}: ${result} ${error}\n${transcript}")
    endif()
endfunction()

check_deadline("" 5000)
foreach(value IN ITEMS 1 3600001 2147483647)
    check_deadline(",\"deadline_ms\":${value}" "${value}")
endforeach()
foreach(value IN ITEMS -1 0 1.5 2147483648 null true "\"forever\"")
    check_deadline(",\"deadline_ms\":${value}" invalid)
endforeach()
