#-------------------------------------------------------------------------------------------------
# MovieBackend unit tests
#
# GoogleTest is fetched at configure time (no system-wide install required, no vcpkg/Conan needed
# for this piece). Pinned to a specific tag so builds are reproducible.
#-------------------------------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)

# Match GoogleTest's runtime library to the rest of the project on MSVC (avoids "mismatched
# /MD vs /MT CRT" link errors).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

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
