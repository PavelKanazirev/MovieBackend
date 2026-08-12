#-------------------------------------------------------------------------------------------------
# moviebackend_core - the library other services/CLIs and the test suite link against.
#-------------------------------------------------------------------------------------------------
moviebackend_add_spdlog()
moviebackend_add_nlohmann_json()

add_library(moviebackend_core
    logging.cpp
    domain_types.cpp
    catalog.cpp
    booking_service.cpp
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

find_package(Threads REQUIRED)

target_link_libraries(moviebackend_core
    PUBLIC
        spdlog::spdlog
        Threads::Threads
    PRIVATE
        nlohmann_json::nlohmann_json
)

moviebackend_set_warnings(moviebackend_core)
moviebackend_enable_clang_tidy(moviebackend_core)

#-------------------------------------------------------------------------------------------------
# moviebackend_net - transport. Cross-platform sockets, message framing, the threaded TCP server,
# and the Protobuf <-> domain request handler land here across the next several commits; this one
# starts it with just the socket wrapper.
#-------------------------------------------------------------------------------------------------
add_library(moviebackend_net
    net/socket.cpp
    net/message_framing.cpp
    net/tcp_server.cpp
    request_handler.cpp
)

target_link_libraries(moviebackend_net
    PUBLIC
        moviebackend_core
        moviebackend_proto
)

target_compile_features(moviebackend_net PUBLIC cxx_std_23)

# Winsock. MSVC picks this up from the #pragma comment in socket.cpp, but MinGW/MSYS2 and
# clang-cl need it stated here.
if(WIN32)
    target_link_libraries(moviebackend_net PUBLIC ws2_32)
endif()

target_link_libraries(moviebackend_net PUBLIC Threads::Threads)

moviebackend_set_warnings(moviebackend_net)
moviebackend_enable_clang_tidy(moviebackend_net)
