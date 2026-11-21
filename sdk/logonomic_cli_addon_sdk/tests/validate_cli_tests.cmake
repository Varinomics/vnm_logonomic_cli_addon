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
