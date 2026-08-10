#!/usr/bin/env bash
#--------------------------------------------------------------------------------------------------
# Pipeline node: BUILD
#
# Configures and compiles the project from scratch-safe state. Safe to run repeatedly; CMake
# reconfigures only when something it depends on changed.
#
#   ./scripts/build.sh                      # Debug build into build/
#   BUILD_TYPE=Release ./scripts/build.sh    # Release build
#   BUILD_DIR=/tmp/mb ./scripts/build.sh     # somewhere else entirely
#
# Extra CMake arguments are passed straight through:
#   ./scripts/build.sh -DMOVIEBACKEND_WARNINGS_AS_ERRORS=ON
#--------------------------------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

log_heading "Build (${BUILD_TYPE})"

require_tool cmake "Install CMake 3.28 or newer: https://cmake.org/download/"

# Ninja is much faster than Make and is what the CMakePresets use, but a machine without it
# should still be able to build rather than being turned away.
generator_args=()
if have_tool ninja; then
    generator_args=(-G Ninja)
    log_info "generator: Ninja"
else
    log_warn "ninja not found; falling back to the default generator (slower)"
fi

log_info "source:     ${REPO_ROOT}"
log_info "build dir:  ${BUILD_DIR}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    "${generator_args[@]}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "$@"

# --parallel with no number lets CMake use every core it can find.
cmake --build "${BUILD_DIR}" --config "${BUILD_TYPE}" --parallel

log_ok "build succeeded: ${BUILD_DIR}"
