#-------------------------------------------------------------------------------------------------
# MovieBackend - top-level CMake build script
#
# This project is an in-memory backend service (C++) for booking movie tickets.
# At this stage only the build/test/lint infrastructure is set up - no production code yet.
#-------------------------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

project(MovieBackend
    VERSION 0.1.0
    DESCRIPTION "In-memory backend service for booking online movie tickets"
    LANGUAGES CXX
)

#-------------------------------------------------------------------------------------------------
# Global project settings
#-------------------------------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standard install subdirectory names (include/, lib/, bin/, ...). Used below by install().
# Explicit rather than relied-upon: CMAKE_INSTALL_INCLUDEDIR currently ends up set anyway, as a
# side effect of GoogleTest's own CMakeLists including this module first - but that's an accident
# of include order, not something to depend on (it silently breaks the day tests are built with
# MOVIEBACKEND_BUILD_TESTS=OFF).
include(GNUInstallDirs)

# Needed by clang-tidy and by VS Code's IntelliSense/clangd to understand the exact build flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# This project uses ordinary headers, not C++20 modules. Left on, CMake adds a module-dependency
# scanning pass to every C++23 translation unit, which costs build time for nothing - and, worse,
# writes GCC-only flags into compile_commands.json that clang-tidy and clangd then reject outright
# as unknown arguments.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Put every runtime artifact in one place. On Windows this is not a nicety but a requirement:
# an .exe only finds its DLLs (once spdlog/protobuf are built as shared libs) if they sit in the
# same directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Default to a Debug build if nothing was requested (single-config generators, e.g. Ninja).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

#-------------------------------------------------------------------------------------------------
# Project-wide options
#-------------------------------------------------------------------------------------------------
option(MOVIEBACKEND_BUILD_TESTS        "Build the unit test suite"                          ON)
option(MOVIEBACKEND_BUILD_SERVER       "Build the standalone TCP server executable"        OFF)
option(MOVIEBACKEND_ENABLE_CLANG_TIDY  "Run clang-tidy as part of compilation"             OFF)
option(MOVIEBACKEND_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"                 OFF)
option(MOVIEBACKEND_FORCE_FETCH_DEPS   "Always build dependencies from source, never reuse
                                        an installed copy"                                 OFF)
option(MOVIEBACKEND_BUILD_DOCS         "Add the 'docs' target (needs Doxygen)"              ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)   # -> moviebackend_set_warnings(<target>)
include(ClangTidy)          # -> moviebackend_enable_clang_tidy(<target>)
include(Dependencies)       # -> moviebackend_add_googletest(), and more to follow

#-------------------------------------------------------------------------------------------------
# Library / executable targets
#-------------------------------------------------------------------------------------------------
add_subdirectory(src)

#-------------------------------------------------------------------------------------------------
# Tests
#-------------------------------------------------------------------------------------------------
if(MOVIEBACKEND_BUILD_TESTS)
    enable_testing()
    moviebackend_add_googletest()
    add_subdirectory(tests)
endif()

#-------------------------------------------------------------------------------------------------
# Documentation ("doc as code": Doxygen + PlantUML, generated from the same headers the
# consumer compiles against, so it can never describe an API that no longer exists).
#-------------------------------------------------------------------------------------------------
if(MOVIEBACKEND_BUILD_DOCS)
    add_subdirectory(docs)
endif()

#-------------------------------------------------------------------------------------------------
# Installation - what `cmake --install` (and therefore the Docker image) actually ships. The
# targets themselves are installed by src/CMakeLists.txt.
#-------------------------------------------------------------------------------------------------
install(DIRECTORY include/moviebackend
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")
