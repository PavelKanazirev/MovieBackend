#!/usr/bin/env bash
#--------------------------------------------------------------------------------------------------
# Shared helpers for the pipeline node scripts.
#
# Sourced, never executed directly. Every script in this directory is a self-contained "node"
# that can be run as a single Jenkins pipeline step (see the Jenkinsfile at the repo root) or by
# a developer at a terminal, with the same behaviour either way.
#
# Conventions every node follows:
#   * exits non-zero on failure, so a CI stage fails loudly rather than silently;
#   * writes machine-readable output (JUnit XML, logs) under build/reports/ for CI to archive;
#   * accepts BUILD_DIR and BUILD_TYPE from the environment, defaulting to a local debug build;
#   * works identically on Linux, WSL and MSYS2/Git Bash on Windows.
#--------------------------------------------------------------------------------------------------

# -e  fail on the first error rather than ploughing on with a broken state
# -u  treat an unset variable as an error, which catches typos in variable names
# -o pipefail  make `a | b` fail if `a` fails, not just if `b` does - without this, piping a
#              build through tee would hide every build failure
set -euo pipefail

#--------------------------------------------------------------------------------------------------
# Locations
#--------------------------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Overridable so CI can build several configurations from one workspace.
BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
REPORT_DIR="${REPORT_DIR:-${BUILD_DIR}/reports}"

#--------------------------------------------------------------------------------------------------
# Output
#
# The markers make each stage's boundaries obvious in a long Jenkins console log.
#--------------------------------------------------------------------------------------------------
log_heading() {
    printf '\n\033[1;34m==> %s\033[0m\n' "$*"
}

log_info() {
    printf '    %s\n' "$*"
}

log_ok() {
    printf '\033[1;32m[ OK ]\033[0m %s\n' "$*"
}

log_warn() {
    printf '\033[1;33m[WARN]\033[0m %s\n' "$*" >&2
}

log_error() {
    printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2
}

#--------------------------------------------------------------------------------------------------
# require_tool <name> [install hint]
#
# Fails with an actionable message rather than a bare "command not found" three lines into a
# pipeline, which is the difference between a two-minute fix and a puzzled bug report.
#--------------------------------------------------------------------------------------------------
require_tool() {
    local tool="$1"
    local hint="${2:-}"

    if ! command -v "${tool}" >/dev/null 2>&1; then
        log_error "required tool '${tool}' is not on PATH"
        [ -n "${hint}" ] && log_info "${hint}"
        return 1
    fi
}

#--------------------------------------------------------------------------------------------------
# have_tool <name> - true if the tool exists. For optional tooling a node can degrade without.
#--------------------------------------------------------------------------------------------------
have_tool() {
    command -v "$1" >/dev/null 2>&1
}

#--------------------------------------------------------------------------------------------------
# source_files - every C++ file this project owns, NUL-separated.
#
# Explicitly limited to src/, include/ and tests/: generated Protobuf code and FetchContent
# dependencies are not ours to format or lint, and including them would bury real findings.
#--------------------------------------------------------------------------------------------------
source_files() {
    find "${REPO_ROOT}/src" "${REPO_ROOT}/include" "${REPO_ROOT}/tests" \
        -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0
}

ensure_report_dir() {
    mkdir -p "${REPORT_DIR}"
}
