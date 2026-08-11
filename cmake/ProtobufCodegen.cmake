#-------------------------------------------------------------------------------------------------
# moviebackend_add_proto_library(<target>
#                                PROTO_ROOT <dir>
#                                PROTOS     <file> [<file> ...]
#                                [PYTHON_OUT <dir>])
#
# Compiles .proto files with `protoc` and wraps the generated C++ sources in a static library
# called <target>, which links the Protobuf runtime and exposes the generated headers to anything
# that links it.
#
# Why this exists instead of protobuf_generate_cpp()/protobuf_generate():
#   Protobuf ships two different CMake integrations - CMake's own FindProtobuf module (which is
#   how Debian/Ubuntu's libprotobuf-dev is found) provides protobuf_generate_cpp(), while
#   protobuf >= 3.22's own config package provides protobuf_generate() with an incompatible
#   signature. Rather than branch on which one happened to be found, this helper drives protoc
#   directly through add_custom_command: one code path, identical behaviour on Ubuntu, MSYS2,
#   vcpkg and a FetchContent-built protobuf.
#
# PYTHON_OUT additionally emits the Python bindings the CLI client imports, from the very same
# invocation - which is the point of using an IDL: the C++ server and the Python client cannot
# drift apart, because both are generated from one file.
#-------------------------------------------------------------------------------------------------
function(moviebackend_add_proto_library target)
    cmake_parse_arguments(ARG "" "PROTO_ROOT;PYTHON_OUT" "PROTOS" ${ARGN})

    if(NOT ARG_PROTO_ROOT)
        message(FATAL_ERROR "moviebackend_add_proto_library(${target}): PROTO_ROOT is required")
    endif()
    if(NOT ARG_PROTOS)
        message(FATAL_ERROR "moviebackend_add_proto_library(${target}): PROTOS is required")
    endif()

    # Locate protoc. Prefer the imported target (correct even for a cross-build or a
    # FetchContent-built protobuf, where the binary lives inside the build tree).
    if(TARGET protobuf::protoc)
        set(_protoc protobuf::protoc)
    elseif(Protobuf_PROTOC_EXECUTABLE)
        set(_protoc "${Protobuf_PROTOC_EXECUTABLE}")
    else()
        message(FATAL_ERROR
            "moviebackend_add_proto_library(${target}): no protoc found. Install the Protobuf "
            "compiler, or configure with -DMOVIEBACKEND_FORCE_FETCH_DEPS=ON to build it from source.")
    endif()

    set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_generated_dir}")

    if(ARG_PYTHON_OUT)
        file(MAKE_DIRECTORY "${ARG_PYTHON_OUT}")
    endif()

    set(_generated_sources "")
    set(_generated_headers "")

    foreach(_proto IN LISTS ARG_PROTOS)
        get_filename_component(_proto_abs "${_proto}" ABSOLUTE)
        get_filename_component(_proto_name "${_proto}" NAME_WE)

        set(_cpp_out "${_generated_dir}/${_proto_name}.pb.cc")
        set(_hpp_out "${_generated_dir}/${_proto_name}.pb.h")

        # Each flag is one list element, with no embedded quotes: VERBATIM quotes them correctly
        # for the shell, so paths containing spaces work while the quotes themselves are not
        # passed through to protoc as part of the path.
        set(_protoc_args "--proto_path=${ARG_PROTO_ROOT}" "--cpp_out=${_generated_dir}")
        set(_outputs "${_cpp_out}" "${_hpp_out}")

        if(ARG_PYTHON_OUT)
            list(APPEND _protoc_args "--python_out=${ARG_PYTHON_OUT}")
            list(APPEND _outputs "${ARG_PYTHON_OUT}/${_proto_name}_pb2.py")
        endif()

        add_custom_command(
            OUTPUT  ${_outputs}
            COMMAND ${_protoc} ${_protoc_args} "${_proto_abs}"
            DEPENDS "${_proto_abs}" ${_protoc}
            COMMENT "protoc: generating sources from ${_proto_name}.proto"
            VERBATIM
        )

        list(APPEND _generated_sources "${_cpp_out}")
        list(APPEND _generated_headers "${_hpp_out}")
        if(ARG_PYTHON_OUT)
            list(APPEND _generated_sources "${ARG_PYTHON_OUT}/${_proto_name}_pb2.py")
        endif()
    endforeach()

    add_library(${target} STATIC ${_generated_sources} ${_generated_headers})

    target_include_directories(${target} SYSTEM PUBLIC "${_generated_dir}")
    target_link_libraries(${target} PUBLIC protobuf::libprotobuf)

    # Generated code is not ours: it is exempt from the project's warning set and from
    # clang-tidy, both of which would only produce noise nobody can act on. Silence the specific
    # warnings that protoc's output is known to trip.
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()

    # Protobuf's generated headers use exceptions and RTTI and expect the same C++ standard as
    # the rest of the project.
    target_compile_features(${target} PUBLIC cxx_std_23)
endfunction()
