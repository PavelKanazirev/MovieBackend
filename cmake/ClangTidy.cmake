#-------------------------------------------------------------------------------------------------
# moviebackend_enable_clang_tidy(<target>)
#
# Looks for a clang-tidy executable in PATH and, if found, wires it into the build so that
# <target> is checked by clang-tidy while compiling (using the rules in .clang-tidy at the repo
# root).
#
# Applied per-target (rather than globally via CMAKE_CXX_CLANG_TIDY) so that third-party
# dependencies pulled in via FetchContent - GoogleTest in particular - are never linted; we don't
# control that code, so warnings there would just be noise. Call this for each of our own targets
# (moviebackend_core, moviebackend_tests, ...), not for third-party ones.
#
# Only does anything when MOVIEBACKEND_ENABLE_CLANG_TIDY is ON, since running clang-tidy on every
# build slows down compilation noticeably.
#-------------------------------------------------------------------------------------------------
function(moviebackend_enable_clang_tidy target)
    if(NOT MOVIEBACKEND_ENABLE_CLANG_TIDY)
        return()
    endif()

    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")

    if(CLANG_TIDY_EXE)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING
            "MOVIEBACKEND_ENABLE_CLANG_TIDY is ON but no clang-tidy executable was found in PATH. "
            "Static analysis will be skipped for ${target}.")
    endif()
endfunction()
