execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -DATLAS_CONSUMER_BUILD_JOBS=0
        -DATLAS_VALIDATE_CONSUMER_BUILD_JOBS_ONLY=ON
        -P "${ATLAS_CONSUMER_SCRIPT}"
    RESULT_VARIABLE validation_result
    OUTPUT_VARIABLE validation_output
    ERROR_VARIABLE validation_error
)

set(validation_log "${validation_output}\n${validation_error}")
if(validation_result EQUAL 0)
    message(FATAL_ERROR "Invalid consumer build job count was accepted")
endif()
if(NOT validation_log MATCHES
   "ATLAS_CONSUMER_BUILD_JOBS must be a positive integer")
    message(FATAL_ERROR
        "Invalid job count failed for the wrong reason:\n${validation_log}"
    )
endif()
