#-------------------------------------------------------------------------------------------------
# moviebackend_set_warnings(<target>)
#
# Applies a consistent, reasonably strict set of compiler warnings to <target>, for both
# MSVC-style and GCC/Clang-style compilers. Controlled by the MOVIEBACKEND_WARNINGS_AS_ERRORS
# cache option defined in the top-level CMakeLists.txt.
#-------------------------------------------------------------------------------------------------
function(moviebackend_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            $<$<BOOL:${MOVIEBACKEND_WARNINGS_AS_ERRORS}>:/WX>
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            $<$<BOOL:${MOVIEBACKEND_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()
endfunction()
