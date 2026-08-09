#-------------------------------------------------------------------------------------------------
# MovieBackend unit tests
#
# GoogleTest is fetched at configure time by moviebackend_add_googletest() (cmake/Dependencies.cmake),
# called from the top-level CMakeLists.txt before this directory is processed. No system-wide
# install is required.
#-------------------------------------------------------------------------------------------------
add_executable(moviebackend_tests
    sanity_test.cpp
    seat_availability_test.cpp
)

target_link_libraries(moviebackend_tests
    PRIVATE
        moviebackend_core
        GTest::gtest_main
)

moviebackend_set_warnings(moviebackend_tests)
moviebackend_enable_clang_tidy(moviebackend_tests)

include(GoogleTest)
gtest_discover_tests(moviebackend_tests)
