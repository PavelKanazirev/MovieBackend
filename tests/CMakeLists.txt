#-------------------------------------------------------------------------------------------------
# MovieBackend unit and integration test suite.
#
# GoogleTest is fetched at configure time by moviebackend_add_googletest() in the top-level
# CMakeLists.txt, so no system-wide install is required.
#
# Two executables rather than one:
#   moviebackend_unit_tests        - fast, deterministic, no sockets. Run these constantly.
#   moviebackend_integration_tests - opens real loopback connections and (from a later commit)
#                                    starts real servers.
#
# Splitting them means `ctest -L unit` gives a sub-second signal while iterating, and it keeps the
# slower, more environment-sensitive tests separable as distinct stages in CI.
#-------------------------------------------------------------------------------------------------
include(GoogleTest)

#-------------------------------------------------------------------------------------------------
# Unit tests
#-------------------------------------------------------------------------------------------------
add_executable(moviebackend_unit_tests
    booking_service_concurrency_test.cpp
    booking_service_test.cpp
    catalog_test.cpp
    logging_test.cpp
    request_handler_test.cpp
)

target_link_libraries(moviebackend_unit_tests
    PRIVATE
        moviebackend_net    # brings moviebackend_core with it
        GTest::gtest_main
)

moviebackend_set_warnings(moviebackend_unit_tests)
moviebackend_enable_clang_tidy(moviebackend_unit_tests)

gtest_discover_tests(moviebackend_unit_tests
    PROPERTIES LABELS "unit"
    # The concurrency tests do real work on 16 threads; the default 5 s is too tight on a loaded
    # CI machine and produces failures that say nothing about the code.
    DISCOVERY_TIMEOUT 30
)

#-------------------------------------------------------------------------------------------------
# Integration tests
#-------------------------------------------------------------------------------------------------
add_executable(moviebackend_integration_tests
    message_framing_test.cpp
    tcp_server_integration_test.cpp
)

target_link_libraries(moviebackend_integration_tests
    PRIVATE
        moviebackend_net
        GTest::gtest_main
)

moviebackend_set_warnings(moviebackend_integration_tests)
moviebackend_enable_clang_tidy(moviebackend_integration_tests)

gtest_discover_tests(moviebackend_integration_tests
    PROPERTIES LABELS "integration"
    DISCOVERY_TIMEOUT 30
)
