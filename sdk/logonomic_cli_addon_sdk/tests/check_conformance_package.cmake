# Reindex each edited fixture so the conformance result measures its declaration.
function(check_package name directory expected)
    execute_process(COMMAND "${INDEX_TOOL}" "${directory}"
        --third-party assets/mark.svg notices/mark-license.txt provenance/mark.json
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: indexing failed: ${output}${error}")
    endif()
    execute_process(COMMAND "${VALIDATE_TOOL}" "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: production validator rejected opaque payload: ${output}${error}")
    endif()
    execute_process(COMMAND "${CONFORMANCE_TOOL}" "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${TEST_ROOT}/${name}.log" "${output}${error}")
    if(NOT result EQUAL expected)
        message(FATAL_ERROR "${name}: expected conformance exit ${expected}, got ${result}: ${output}${error}")
    endif()
    foreach(check IN LISTS ARGN)
        string(FIND "${output}" "${check}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${name}: missing conformance result '${check}': ${output}${error}")
        endif()
    endforeach()
    message(STATUS "${name}: validator accepted and conformance returned the expected checks and exit ${result}")
endfunction()
