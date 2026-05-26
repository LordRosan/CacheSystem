cmake_minimum_required(VERSION 3.24)

project(
    CacheSystem
    VERSION 0.1.0
    DESCRIPTION "A compact C++20 cache library with LRU, TTL, synchronized, and sharded variants"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(CACHESYSTEM_BUILD_BENCHMARK "Build the standalone Release benchmark when test.cpp is present" ${PROJECT_IS_TOP_LEVEL})
option(CACHESYSTEM_BUILD_TESTS "Build standalone correctness and package validation tests" ${PROJECT_IS_TOP_LEVEL})
option(CACHESYSTEM_WARNINGS_AS_ERRORS "Treat CacheSystem warnings as errors" OFF)

function(cachesystem_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CACHESYSTEM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(CACHESYSTEM_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(cache_system STATIC
    source/cache_system/version.cpp
)
add_library(CacheSystem::cache_system ALIAS cache_system)

target_compile_features(cache_system PUBLIC cxx_std_20)
target_include_directories(cache_system
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(cache_system
    PUBLIC
        CACHE_SYSTEM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        CACHE_SYSTEM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        CACHE_SYSTEM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

cachesystem_apply_warnings(cache_system)
target_compile_definitions(cache_system PRIVATE $<$<CONFIG:Release>:NDEBUG>)
target_compile_options(cache_system PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>
)

if(PROJECT_IS_TOP_LEVEL AND CACHESYSTEM_BUILD_BENCHMARK AND EXISTS "${PROJECT_SOURCE_DIR}/test.cpp")
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "CacheSystem benchmark must be configured with -DCMAKE_BUILD_TYPE=Release. Use -DCACHESYSTEM_BUILD_BENCHMARK=OFF for non-Release library-only builds.")
    endif()

    file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/results" CACHESYSTEM_RESULTS_DIR)
    add_executable(cache_system_benchmark test.cpp)
    target_link_libraries(cache_system_benchmark PRIVATE CacheSystem::cache_system)
    target_compile_features(cache_system_benchmark PRIVATE cxx_std_20)
    target_compile_definitions(cache_system_benchmark PRIVATE CACHE_SYSTEM_RESULTS_DIR="${CACHESYSTEM_RESULTS_DIR}")
    cachesystem_apply_warnings(cache_system_benchmark)
    target_compile_definitions(cache_system_benchmark PRIVATE $<$<CONFIG:Release>:NDEBUG>)
    target_compile_options(cache_system_benchmark PRIVATE
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>
    )
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND CACHESYSTEM_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/test.cpp")
    add_executable(cache_system_correctness_tests test.cpp)
    target_link_libraries(cache_system_correctness_tests PRIVATE CacheSystem::cache_system)
    target_compile_features(cache_system_correctness_tests PRIVATE cxx_std_20)
    target_compile_definitions(cache_system_correctness_tests PRIVATE CACHE_SYSTEM_CORRECTNESS_ONLY=1)
    cachesystem_apply_warnings(cache_system_correctness_tests)
    add_test(NAME cache_system_correctness COMMAND cache_system_correctness_tests)

    set(CACHESYSTEM_PACKAGE_VALIDATION_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/package-validation-install")
    add_test(
        NAME cache_system_package_validation
        COMMAND
            ${CMAKE_COMMAND}
            "-DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}"
            "-DPROJECT_BINARY_DIR=${CMAKE_BINARY_DIR}"
            "-DINSTALL_PREFIX=${CACHESYSTEM_PACKAGE_VALIDATION_INSTALL_PREFIX}"
            "-DPACKAGE_VALIDATION_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/package_validation"
            "-DPACKAGE_VALIDATION_BINARY_DIR=${CMAKE_BINARY_DIR}/package-validation-build"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DSMOKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${PROJECT_SOURCE_DIR}/cmake/run_package_validation.cmake"
    )
endif()

install(TARGETS cache_system
    EXPORT CacheSystemTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/CacheSystemConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/CacheSystemConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CacheSystem
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/CacheSystemConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/CacheSystemConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CacheSystemConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CacheSystem
)
install(EXPORT CacheSystemTargets
    NAMESPACE CacheSystem::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CacheSystem
)
