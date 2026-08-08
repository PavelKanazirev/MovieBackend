#-------------------------------------------------------------------------------------------------
# moviebackend_core - the library other services/CLIs and the test suite link against.
#-------------------------------------------------------------------------------------------------
add_library(moviebackend_core
    seat_availability.cpp
)

target_include_directories(moviebackend_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(moviebackend_core PUBLIC cxx_std_23)

moviebackend_set_warnings(moviebackend_core)
moviebackend_enable_clang_tidy(moviebackend_core)
