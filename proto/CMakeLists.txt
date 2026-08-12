#-------------------------------------------------------------------------------------------------
# moviebackend_proto - the generated message classes for the client/server protocol.
#
# booking.proto is the single source of truth for the wire format. This target compiles it to
# C++ for the server, and at the same time emits the Python bindings the CLI client imports
# (into cli/moviebackend_cli/generated/), so client and server are always generated from the
# same revision of the IDL.
#-------------------------------------------------------------------------------------------------
moviebackend_add_proto_library(moviebackend_proto
    PROTO_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}"
    PROTOS      "${CMAKE_CURRENT_SOURCE_DIR}/booking.proto"
    PYTHON_OUT  "${CMAKE_SOURCE_DIR}/cli/moviebackend_cli/generated"
)
