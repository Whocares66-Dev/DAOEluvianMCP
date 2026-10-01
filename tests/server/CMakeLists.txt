# The listener over loopback: real sockets, no game. Built with the plugin's
# presets, since the core ones have no server. Catch2 is found again here:
# vcpkg's imported targets are visible only below where they were found,
# and this folder is added from the top, after el_server exists.
find_package(Catch2 3 CONFIG REQUIRED)

add_executable(el_server_tests ../quiet_crt.cpp test_server.cpp)
el_apply_warnings(el_server_tests)
target_link_libraries(el_server_tests PRIVATE el_server Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(el_server_tests)
