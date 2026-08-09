#!/usr/bin/env bash
#--------------------------------------------------------------------------------------------------
# Pipeline node: DOCUMENTATION
#
# Renders the PlantUML diagrams to SVG, then runs Doxygen over the headers to produce the API
# reference. Output lands in ${BUILD_DIR}/docs/html/, ready for Jenkins to publish as an HTML
# artifact.
#
#   ./scripts/generate_docs.sh
#
# Both tools are optional. Missing PlantUML costs you the diagrams; missing Doxygen costs you the
# reference. Neither is allowed to fail the pipeline on a machine that simply lacks the tool -
# that is what the --strict flag is for, which CI passes.
#--------------------------------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

strict=0
[ "${1:-}" = "--strict" ] && strict=1

log_heading "Documentation"

diagram_dir="${REPO_ROOT}/docs/diagrams"
diagram_out="${BUILD_DIR}/docs/html/diagrams"

missing_tool() {
    if [ "${strict}" -eq 1 ]; then
        log_error "$1"
        exit 1
    fi
    log_warn "$1"
}

#--------------------------------------------------------------------------------------------------
# 1. Diagrams
#--------------------------------------------------------------------------------------------------
if have_tool plantuml; then
    mkdir -p "${diagram_out}"
    log_info "rendering $(ls -1 "${diagram_dir}"/*.puml 2>/dev/null | wc -l) PlantUML diagram(s)"

    # SVG rather than PNG: the sequence diagrams are wide, and vector output stays readable when
    # a reader zooms in on one interaction.
    plantuml -tsvg -o "${diagram_out}" "${diagram_dir}"/*.puml
    log_ok "diagrams rendered to ${diagram_out}"
else
    missing_tool "plantuml not found; diagrams will not be rendered (sudo apt install plantuml)"
fi

#--------------------------------------------------------------------------------------------------
# 2. API reference
#--------------------------------------------------------------------------------------------------
if have_tool doxygen; then
    if [ ! -d "${BUILD_DIR}" ]; then
        log_error "no build directory at ${BUILD_DIR}; run ./scripts/build.sh first"
        exit 1
    fi

    cmake --build "${BUILD_DIR}" --target docs
    log_ok "API reference at ${BUILD_DIR}/docs/html/index.html"
else
    missing_tool "doxygen not found; the API reference will not be generated (sudo apt install doxygen graphviz)"
fi
