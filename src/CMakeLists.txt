#-------------------------------------------------------------------------------------------------
# moviebackend_core - the library other services/CLIs and the test suite link against.
#-------------------------------------------------------------------------------------------------
moviebackend_add_spdlog()
moviebackend_add_nlohmann_json()

add_library(moviebackend_core
    logging.cpp
    domain_types.cpp
    catalog.cpp
)

target_include_directories(moviebackend_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(moviebackend_core PUBLIC cxx_std_23)

# logging.hpp requires SPDLOG_ACTIVE_LEVEL to be set before spdlog.h is included, and any target
# that links moviebackend_core (or otherwise includes the header) needs the same definition - so
# this is PUBLIC. Debug builds keep everything down to trace; Release builds compile trace/debug
# statements out entirely (see logging.hpp's header comment for why that split exists).
target_compile_definitions(moviebackend_core
    PUBLIC
        SPDLOG_ACTIVE_LEVEL=$<IF:$<CONFIG:Debug>,SPDLOG_LEVEL_TRACE,SPDLOG_LEVEL_INFO>
)

target_link_libraries(moviebackend_core
    PUBLIC
        spdlog::spdlog
    PRIVATE
        nlohmann_json::nlohmann_json
)

moviebackend_set_warnings(moviebackend_core)
moviebackend_enable_clang_tidy(moviebackend_core)
