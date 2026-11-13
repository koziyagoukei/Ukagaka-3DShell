# Runs EXE with ARGS (a ;-list) and checks the exit code equals EXPECTED.
execute_process(COMMAND "${EXE}" ${ARGS}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
message("exit=${result}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
if(NOT "${result}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "expected exit code ${EXPECTED}, got ${result}")
endif()
