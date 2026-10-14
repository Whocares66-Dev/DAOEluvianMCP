# gh_tests: ghilana's tools and readers over files made in the test, and
# over this machine's install where it has one ([install], skipped where
# not). Catch2 comes as tests/CMakeLists.txt found or fetched it.
add_executable(
    gh_tests
    ../quiet_crt.cpp
    test_formats.cpp
    test_install.cpp
    test_models.cpp
    test_tools.cpp
)
el_apply_warnings(gh_tests)
target_link_libraries(gh_tests PRIVATE gh_lib Catch2::Catch2WithMain)
# DAOLib's tests/files_builder.h, which Build.h includes, is put on its
# path by cmake/Ghilana.cmake, once DAOLib is found.
target_include_directories(gh_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

catch_discover_tests(gh_tests TEST_PREFIX "ghilana: ")
