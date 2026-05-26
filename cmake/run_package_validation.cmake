if(NOT DEFINED CMAKE_COMMAND_PATH)
    message(FATAL_ERROR "CMAKE_COMMAND_PATH is required")
endif()
if(NOT DEFINED PROJECT_BINARY_DIR)
    message(FATAL_ERROR "PROJECT_BINARY_DIR is required")
endif()
if(NOT DEFINED INSTALL_PREFIX)
    message(FATAL_ERROR "INSTALL_PREFIX is required")
endif()
if(NOT DEFINED PACKAGE_VALIDATION_SOURCE_DIR)
    message(FATAL_ERROR "PACKAGE_VALIDATION_SOURCE_DIR is required")
endif()
if(NOT DEFINED PACKAGE_VALIDATION_BINARY_DIR)
    message(FATAL_ERROR "PACKAGE_VALIDATION_BINARY_DIR is required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" --install "${PROJECT_BINARY_DIR}" --prefix "${INSTALL_PREFIX}"
    RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "CacheSystem install step failed with exit code ${install_result}")
endif()

file(REMOVE_RECURSE "${PACKAGE_VALIDATION_BINARY_DIR}")

set(configure_command
    "${CMAKE_COMMAND_PATH}"
    -S "${PACKAGE_VALIDATION_SOURCE_DIR}"
    -B "${PACKAGE_VALIDATION_BINARY_DIR}"
    -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
)
if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
    list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
if(DEFINED SMOKE_BUILD_TYPE AND NOT SMOKE_BUILD_TYPE STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${SMOKE_BUILD_TYPE}")
endif()

execute_process(COMMAND ${configure_command} RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "CacheSystem package validation configure failed with exit code ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND_PATH}" --build "${PACKAGE_VALIDATION_BINARY_DIR}"
    RESULT_VARIABLE build_result
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "CacheSystem package validation build failed with exit code ${build_result}")
endif()

execute_process(
    COMMAND "${PACKAGE_VALIDATION_BINARY_DIR}/package_validation${EXECUTABLE_SUFFIX}"
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "CacheSystem package validation executable failed with exit code ${run_result}")
endif()
