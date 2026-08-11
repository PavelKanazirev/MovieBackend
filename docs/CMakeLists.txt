#-------------------------------------------------------------------------------------------------
# Documentation as code.
#
# Adds a `docs` target that renders the API reference straight from the Doxygen comments in
# include/moviebackend/, together with the hand-written pages in docs/ and the PlantUML diagrams
# in docs/diagrams/.
#
# Generating from the headers is the whole point: the documentation a consumer reads is produced
# from the same declarations the compiler checks, so it cannot describe a function that no longer
# exists or a parameter that was renamed.
#
#   cmake --build --preset debug --target docs
#   # then open out/build/debug/docs/html/index.html
#
# Doxygen is optional. When it is not installed the target is simply not created, with a message
# saying so - a missing documentation tool must never break a code build.
#-------------------------------------------------------------------------------------------------
find_package(Doxygen QUIET COMPONENTS dot)

if(NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found - the 'docs' target will not be available. "
                   "Install it with 'sudo apt install doxygen graphviz' (Linux) or "
                   "'winget install DimitriVanHeesch.Doxygen' (Windows).")
    return()
endif()

#-------------------------------------------------------------------------------------------------
# PlantUML.
#
# Doxygen renders \startuml blocks itself when told where plantuml.jar lives. If it is absent the
# diagrams degrade to their source text rather than failing the build, and the .puml files under
# docs/diagrams/ can still be rendered by hand or by scripts/generate_docs.sh.
#-------------------------------------------------------------------------------------------------
find_file(PLANTUML_JAR_PATH
    NAMES plantuml.jar
    PATHS /usr/share/plantuml /usr/local/share/plantuml /opt/plantuml "$ENV{PLANTUML_HOME}"
    DOC "Path to plantuml.jar, used by Doxygen to render \\startuml blocks"
)

if(PLANTUML_JAR_PATH)
    message(STATUS "PlantUML: ${PLANTUML_JAR_PATH}")
else()
    message(STATUS "PlantUML jar not found - diagrams in doc comments will not be rendered. "
                   "Set -DPLANTUML_JAR_PATH=/path/to/plantuml.jar to enable them.")
    set(PLANTUML_JAR_PATH "")
endif()

# Doxygen's config file wants the literal strings YES/NO, not CMake's TRUE/FALSE.
if(TARGET Doxygen::dot)
    set(DOXYGEN_DOT_FOUND "YES")
else()
    set(DOXYGEN_DOT_FOUND "NO")
    message(STATUS "Graphviz 'dot' not found - class and include graphs will be omitted.")
endif()

set(DOXYGEN_INPUT_DIRS "${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}")
set(DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
# docs/mainpage.md rather than the README.
#
# The README is written for GitHub and carries a table of contents built from markdown anchor
# links. Doxygen parses those as \ref commands, cannot resolve them, and emits a warning per
# entry - which would either bury real documentation warnings or force anchor syntax into the
# README that GitHub renders as literal text. A short Doxygen-specific landing page avoids both,
# and lets the API reference open on what an API reader actually needs.
set(DOXYGEN_MAIN_PAGE  "${CMAKE_CURRENT_SOURCE_DIR}/mainpage.md")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    @ONLY
)

add_custom_target(docs
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM
)

message(STATUS "Documentation: build the 'docs' target, then open "
               "${CMAKE_CURRENT_BINARY_DIR}/html/index.html")
