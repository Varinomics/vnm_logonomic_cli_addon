cmake_minimum_required(VERSION 3.21)

foreach(required IN ITEMS STAGE_TOOL VALIDATE_TOOL INDEX_TOOL FIXTURE_PACKAGES TEST_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(package_id "org.example.unknown-fixture")
set(source_package "${FIXTURE_PACKAGES}/${package_id}")

function(run_success)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()

function(generation_digest directory output_variable)
    execute_process(COMMAND "${VALIDATE_TOOL}" "${directory}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0 OR NOT output MATCHES "^OK [^ ]+ ([0-9a-f]+) bytes=")
        message(FATAL_ERROR "Cannot validate fixture: ${output}\n${error}")
    endif()
    set(${output_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(write_input path)
    file(WRITE "${path}" "{\"artifact_set_version\":1,\"packages\":[${ARGN}]}")
endfunction()

function(expect_rejected name reason input)
    set(output_root "${TEST_ROOT}/${name}")
    execute_process(COMMAND "${STAGE_TOOL}" --output "${output_root}" --input "${input}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL ${reason} ")
        message(FATAL_ERROR "${name} did not reject with ${reason}: ${result}\n${output}\n${error}")
    endif()
    if(EXISTS "${output_root}")
        message(FATAL_ERROR "${name} published a failed payload")
    endif()
endfunction()

# Oracle: the package contract's generation digest binds the complete input
# bytes; the staging contract publishes exactly the supplied package set.
generation_digest("${source_package}" original_digest)
set(original_entry "{\"directory\":\"${source_package}\",\"generation_digest\":\"${original_digest}\"}")
set(input "${TEST_ROOT}/input.json")
write_input("${input}" "${original_entry}")
run_success("${STAGE_TOOL}" --output "${TEST_ROOT}/single" --input "${input}"
    --evidence "${TEST_ROOT}/single.evidence.json")
run_success("${STAGE_TOOL}" --verify "${TEST_ROOT}/single"
    --evidence "${TEST_ROOT}/single.evidence.json")
file(READ "${TEST_ROOT}/single.evidence.json" evidence)
string(JSON receipt_package_id GET "${evidence}" packages 0 package_id)
string(JSON receipt_digest GET "${evidence}" packages 0 generation_digest)
if(NOT receipt_package_id STREQUAL package_id OR NOT receipt_digest STREQUAL original_digest)
    message(FATAL_ERROR "Receipt does not bind the validated package identity")
endif()
file(WRITE "${TEST_ROOT}/tampered.evidence.json" "${evidence} ")
execute_process(COMMAND "${STAGE_TOOL}" --verify "${TEST_ROOT}/single"
    --evidence "${TEST_ROOT}/tampered.evidence.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL evidence_mismatch " OR NOT output STREQUAL "")
    message(FATAL_ERROR "Verifier trusted a noncanonical receipt: ${output}\n${error}")
endif()
generation_digest("${TEST_ROOT}/single/${package_id}" staged_digest)
if(NOT staged_digest STREQUAL original_digest)
    message(FATAL_ERROR "Staging changed the package generation")
endif()
file(GLOB staged_entries RELATIVE "${TEST_ROOT}/single" "${TEST_ROOT}/single/*")
if(NOT staged_entries STREQUAL package_id)
    message(FATAL_ERROR "Staging did not publish exactly the supplied package set")
endif()

run_success("${STAGE_TOOL}" --output "${TEST_ROOT}/zero" --evidence "${TEST_ROOT}/zero.evidence.json")
run_success("${STAGE_TOOL}" --verify "${TEST_ROOT}/zero" --evidence "${TEST_ROOT}/zero.evidence.json")
file(REMOVE_RECURSE "${TEST_ROOT}/zero")
run_success("${STAGE_TOOL}" --verify "${TEST_ROOT}/zero" --evidence "${TEST_ROOT}/zero.evidence.json")
execute_process(COMMAND "${STAGE_TOOL}" --verify "${TEST_ROOT}/zero"
    --evidence "${TEST_ROOT}/single.evidence.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL evidence_mismatch " OR NOT output STREQUAL "")
    message(FATAL_ERROR "Absent package root was accepted with nonempty evidence")
endif()
file(WRITE "${TEST_ROOT}/zero" "not a directory")
execute_process(COMMAND "${STAGE_TOOL}" --verify "${TEST_ROOT}/zero"
    --evidence "${TEST_ROOT}/zero.evidence.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL invalid_staged_root " OR NOT output STREQUAL "")
    message(FATAL_ERROR "Non-directory package root was accepted as empty")
endif()
write_input("${input}")
run_success("${STAGE_TOOL}" --output "${TEST_ROOT}/explicit-zero" --input "${input}")
file(GLOB zero_entries "${TEST_ROOT}/explicit-zero/*")
if(zero_entries)
    message(FATAL_ERROR "Zero packages produced package content")
endif()

write_input("${input}" "${original_entry},${original_entry}")
expect_rejected(duplicate duplicate_package_id "${input}")
string(REPEAT "0" 64 wrong_digest)
write_input("${input}" "{\"directory\":\"${source_package}\",\"generation_digest\":\"${wrong_digest}\"}")
expect_rejected(wrong-digest digest_mismatch "${input}")

file(COPY "${source_package}/" DESTINATION "${TEST_ROOT}/replacement")
file(READ "${TEST_ROOT}/replacement/logonomic-cli-addon.json" bootstrap)
string(JSON bootstrap SET "${bootstrap}" release_version "\"9.8.1\"")
file(WRITE "${TEST_ROOT}/replacement/logonomic-cli-addon.json" "${bootstrap}")
write_input("${input}"
    "{\"directory\":\"${TEST_ROOT}/replacement\",\"generation_digest\":\"${original_digest}\"}")
expect_rejected(mixed-generation digest_mismatch "${input}")

file(WRITE "${TEST_ROOT}/replacement/capabilities/profile-storage@2.json"
    "{\"capability_id\":\"profile-storage\",\"version\":2,\"payload\":{\"opaque\":\"preserved\"}}")
file(WRITE "${TEST_ROOT}/replacement/assets/über space;name.txt" "opaque package resource\n")
run_success("${INDEX_TOOL}" "${TEST_ROOT}/replacement")
generation_digest("${TEST_ROOT}/replacement" replacement_digest)
write_input("${input}"
    "{\"directory\":\"${TEST_ROOT}/replacement\",\"generation_digest\":\"${replacement_digest}\"}")
run_success("${STAGE_TOOL}" --output "${TEST_ROOT}/future" --input "${input}"
    --evidence "${TEST_ROOT}/future.evidence.json")
run_success("${STAGE_TOOL}" --verify "${TEST_ROOT}/future"
    --evidence "${TEST_ROOT}/future.evidence.json")
file(SHA256 "${TEST_ROOT}/replacement/capabilities/profile-storage@2.json" input_hash)
file(SHA256 "${TEST_ROOT}/future/${package_id}/capabilities/profile-storage@2.json" output_hash)
if(NOT input_hash STREQUAL output_hash)
    message(FATAL_ERROR "Unsupported optional capability bytes were not preserved")
endif()
file(READ "${TEST_ROOT}/future/${package_id}/assets/über space;name.txt" preserved_asset)
if(NOT preserved_asset STREQUAL "opaque package resource\n")
    message(FATAL_ERROR "Stager changed a valid package-relative path")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    execute_process(COMMAND stat -c %a
        "${TEST_ROOT}/future/${package_id}/adapter/unknown-fixture-adapter"
        RESULT_VARIABLE result OUTPUT_VARIABLE mode OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0 OR NOT mode STREQUAL "755")
        message(FATAL_ERROR "Staged adapter is not executable with canonical permissions")
    endif()
endif()
file(APPEND "${TEST_ROOT}/replacement/adapter/unknown-fixture-adapter${CMAKE_EXECUTABLE_SUFFIX}" "changed")
expect_rejected(corrupt-file digest_mismatch "${input}")

set(other_package "${FIXTURE_PACKAGES}/org.example.unknown-fixture-third-party-ok")
generation_digest("${other_package}" other_digest)
write_input("${input}"
    "${original_entry},{\"directory\":\"${other_package}\",\"generation_digest\":\"${other_digest}\"}")
run_success("${STAGE_TOOL}" --output "${TEST_ROOT}/two" --input "${input}")
generation_digest("${TEST_ROOT}/two/org.example.unknown-fixture-third-party-ok" staged_other_digest)
if(NOT staged_other_digest STREQUAL other_digest)
    message(FATAL_ERROR "Third-party notice/provenance closure changed")
endif()

file(WRITE "${TEST_ROOT}/single/sentinel" "existing payload")
execute_process(COMMAND "${STAGE_TOOL}" --verify "${TEST_ROOT}/single"
    --evidence "${TEST_ROOT}/single.evidence.json"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL invalid_staged_root " OR NOT output STREQUAL "")
    message(FATAL_ERROR "Verifier did not reject extra payload content")
endif()
execute_process(COMMAND "${STAGE_TOOL}" --output "${TEST_ROOT}/single"
    RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "FAIL output_exists " OR
   NOT EXISTS "${TEST_ROOT}/single/sentinel")
    message(FATAL_ERROR "Stager did not preserve the existing output")
endif()

foreach(invalid IN ITEMS
        "{\"artifact_set_version\":2,\"packages\":[]}"
        "{\"artifact_set_version\":1,\"packages\":[],\"roster\":[]}"
        "{\"artifact_set_version\":1,\"packages\":[{\"directory\":\"relative\",\"generation_digest\":\"${original_digest}\"}]}"
        "{\"artifact_set_version\":1,\"artifact_set_version\":1,\"packages\":[]}")
    file(WRITE "${input}" "${invalid}")
    expect_rejected(invalid-input invalid_artifact_set "${input}")
endforeach()

file(GLOB temporary_entries "${TEST_ROOT}/.cli-addon-stage-*")
if(temporary_entries)
    message(FATAL_ERROR "Staging left incomplete temporary payloads: ${temporary_entries}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/package_cmake_tests.cmake")
message(STATUS "CLI add-on staging preserves exact generations and rejects invalid artifact sets")
