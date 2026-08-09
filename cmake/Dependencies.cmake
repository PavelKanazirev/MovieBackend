#-------------------------------------------------------------------------------------------------
# Third-party dependency resolution.
#
# Strategy: look for a version already installed on the machine first (fast, and what a
# distro-packaged or vcpkg-provisioned build wants), and only fall back to downloading and
# building it via FetchContent when nothing suitable is found. That keeps a fresh `git clone` +
# `cmake --preset ...` working out of the box on both Linux and Windows (Visual Studio), without
# forcing users who already have a library installed to rebuild it.
#
# Set MOVIEBACKEND_FORCE_FETCH_DEPS=ON to skip the system search entirely (useful when you want a
# fully reproducible, self-contained build).
#
# Dependencies resolved here:
#   GoogleTest    - unit test framework, only when MOVIEBACKEND_BUILD_TESTS is ON (BSD-3-Clause)
#   spdlog        - logging subsystem (MIT)
#   nlohmann_json - JSON parsing for the catalog config file (MIT)
#-------------------------------------------------------------------------------------------------
include(FetchContent)

# Third-party sources are not ours to lint, and their warnings are not ours to fix.
set(FETCHCONTENT_QUIET OFF)

#-------------------------------------------------------------------------------------------------
# GoogleTest - https://github.com/google/googletest
#
# Always fetched rather than taken from the system: test frameworks are very sensitive to ABI and
# standard-library flag mismatches, and pinning the exact version keeps CI reproducible.
#-------------------------------------------------------------------------------------------------
function(moviebackend_add_googletest)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # Match GoogleTest's runtime library to the rest of the project on MSVC (avoids
    # "mismatched /MD vs /MT CRT" link errors).
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endfunction()

#-------------------------------------------------------------------------------------------------
# spdlog - https://github.com/gabime/spdlog
#
# Chosen as the project's log subsystem: permissively licensed, actively maintained, extremely
# well documented, works identically on Windows and Linux, and supports exactly the runtime level
# control the requirements call for (trace/debug for development, error/critical only for release
# builds). Not consumed by anything yet - the logging implementation lands in a later commit.
#-------------------------------------------------------------------------------------------------
function(moviebackend_add_spdlog)
    if(NOT MOVIEBACKEND_FORCE_FETCH_DEPS)
        find_package(spdlog 1.11 QUIET)
        if(spdlog_FOUND)
            message(STATUS "spdlog: using system package ${spdlog_VERSION}")
            return()
        endif()
    endif()

    message(STATUS "spdlog: not found locally, fetching v1.14.1")
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
    )
    # Build spdlog as a compiled library rather than header-only: it will end up included by most
    # translation units in this project, and pre-compiling it keeps our own build times down.
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endfunction()

#-------------------------------------------------------------------------------------------------
# nlohmann_json - https://github.com/nlohmann/json
#
# Header-only JSON reader, for the catalog/config file. Not consumed by anything yet - the
# Catalog implementation lands in a later commit.
#-------------------------------------------------------------------------------------------------
function(moviebackend_add_nlohmann_json)
    if(NOT MOVIEBACKEND_FORCE_FETCH_DEPS)
        find_package(nlohmann_json 3.10 QUIET)
        if(nlohmann_json_FOUND)
            message(STATUS "nlohmann_json: using system package ${nlohmann_json_VERSION}")
            return()
        endif()
    endif()

    message(STATUS "nlohmann_json: not found locally, fetching v3.11.3")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endfunction()
