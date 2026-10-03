# Catch2 comes from vcpkg in the x86 presets (the manifest's `tests`
# feature). core-cov builds with no vcpkg at all, 64-bit with clang-cl, so
# there it is fetched, as DAOTurnBased's core-cov fetches it.
find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found through find_package; fetching it.")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# Over el_core only: HTTP, the protocol, the registry, the bus, the queue,
# with no game and no socket.
add_executable(
    el_tests
    quiet_crt.cpp
    test_args.cpp
    test_base64.cpp
    test_bench.cpp
    test_conversation.cpp
    test_erf.cpp
    test_events.cpp
    test_event_log.cpp
    test_frame_queue.cpp
    test_http.cpp
    test_levelup.cpp
    test_log_tail.cpp
    test_loot.cpp
    test_mcp.cpp
    test_origin.cpp
    test_registry.cpp
    test_rest.cpp
    test_scenario.cpp
    test_travel.cpp
)
el_apply_warnings(el_tests)
target_link_libraries(el_tests PRIVATE el_core Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(el_tests)

# The Python tools' own tests (tools/test_*.py), where there is a Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME python_tools
        COMMAND
            "${Python3_EXECUTABLE}" -m unittest discover -s
            "${PROJECT_SOURCE_DIR}/tools" -p "test_*.py"
    )
endif()

# Coverage, for the core-cov preset:
#
#   .\tools\build.ps1 -Preset core-cov -Coverage
#
# Runs el_tests once as one process (ctest would run each case in its own
# and leave a .profraw per case), and reports src/core, file by file, with
# the line-by-line view as HTML in build\core-cov\coverage\html\.
if(EL_COVERAGE AND EL_LLVM_PROFDATA AND EL_LLVM_COV)
    set(_cov_dir "${CMAKE_BINARY_DIR}/coverage")
    set(_cov_sources "${PROJECT_SOURCE_DIR}/src/core")
    add_custom_target(
        coverage
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cov_dir}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "LLVM_PROFILE_FILE=${_cov_dir}/el_tests.profraw"
            "$<TARGET_FILE:el_tests>"
        COMMAND
            "${EL_LLVM_PROFDATA}" merge -sparse "${_cov_dir}/el_tests.profraw"
            -o "${_cov_dir}/tests.profdata"
        COMMAND
            "${EL_LLVM_COV}" show "$<TARGET_FILE:el_tests>"
            "-instr-profile=${_cov_dir}/tests.profdata" -format=html
            "-output-dir=${_cov_dir}/html" ${_cov_sources}
        COMMAND
            "${EL_LLVM_COV}" report "$<TARGET_FILE:el_tests>"
            "-instr-profile=${_cov_dir}/tests.profdata" ${_cov_sources}
        DEPENDS el_tests
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Coverage of src/core by the tests; HTML in ${_cov_dir}/html"
        VERBATIM
        USES_TERMINAL
    )
elseif(EL_COVERAGE)
    message(
        WARNING
        "EL_COVERAGE is ON but llvm-profdata or llvm-cov was not found; no coverage target"
    )
endif()
