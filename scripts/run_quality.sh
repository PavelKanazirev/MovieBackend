#!/usr/bin/env bash
#--------------------------------------------------------------------------------------------------
# Pipeline node: CODE QUALITY
#
# Three independent checks, all reported before the script exits so one run tells you everything
# that needs fixing rather than only the first thing:
#
#   1. clang-format - is every file formatted to .clang-format?
#   2. clang-tidy   - static analysis against .clang-tidy
#   3. warnings     - a clean rebuild with warnings promoted to errors
#
#   ./scripts/run_quality.sh          # check only; fails if anything is wrong
#   ./scripts/run_quality.sh --fix    # reformat in place, then check the rest
#--------------------------------------------------------------------------------------------------
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

fix_mode=0
[ "${1:-}" = "--fix" ] && fix_mode=1

log_heading "Code quality"
ensure_report_dir

failures=0

#--------------------------------------------------------------------------------------------------
# 1. Formatting
#--------------------------------------------------------------------------------------------------
if have_tool clang-format; then
    log_info "clang-format: $(clang-format --version)"

    if [ "${fix_mode}" -eq 1 ]; then
        source_files | xargs -0 clang-format -i
        log_ok "sources reformatted in place"
    else
        # --dry-run --Werror makes clang-format exit non-zero on the first file that differs,
        # and print the diff, which is exactly what a CI check wants.
        if source_files | xargs -0 clang-format --dry-run --Werror \
                2> "${REPORT_DIR}/clang-format.log"; then
            log_ok "formatting is clean"
        else
            log_error "formatting violations found - see ${REPORT_DIR}/clang-format.log"
            log_info  "fix them with: ./scripts/run_quality.sh --fix"
            failures=$((failures + 1))
        fi
    fi
else
    log_warn "clang-format not found; skipping the formatting check"
fi

#--------------------------------------------------------------------------------------------------
# 2. Static analysis
#
# clang-tidy needs compile_commands.json to know the exact flags each file is built with, which
# is why build.sh always exports it.
#--------------------------------------------------------------------------------------------------
if have_tool clang-tidy; then
    log_info "clang-tidy: $(clang-tidy --version | head -1)"

    if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
        log_error "no compile_commands.json in ${BUILD_DIR}; run ./scripts/build.sh first"
        failures=$((failures + 1))
    else
        # run-clang-tidy parallelises across files; fall back to a serial loop where it is absent
        # (it is a separate package on some distributions).
        if have_tool run-clang-tidy; then
            if run-clang-tidy -p "${BUILD_DIR}" -quiet \
                    "${REPO_ROOT}/src/.*" "${REPO_ROOT}/include/.*" \
                    > "${REPORT_DIR}/clang-tidy.log" 2>&1; then
                log_ok "static analysis is clean"
            else
                log_error "clang-tidy findings - see ${REPORT_DIR}/clang-tidy.log"
                tail -30 "${REPORT_DIR}/clang-tidy.log" >&2
                failures=$((failures + 1))
            fi
        else
            tidy_failed=0
            : > "${REPORT_DIR}/clang-tidy.log"
            while IFS= read -r -d '' file; do
                clang-tidy -p "${BUILD_DIR}" "${file}" \
                    >> "${REPORT_DIR}/clang-tidy.log" 2>&1 || tidy_failed=1
            done < <(find "${REPO_ROOT}/src" "${REPO_ROOT}/include" -name '*.cpp' -print0)

            if [ "${tidy_failed}" -eq 0 ]; then
                log_ok "static analysis is clean"
            else
                log_error "clang-tidy findings - see ${REPORT_DIR}/clang-tidy.log"
                failures=$((failures + 1))
            fi
        fi
    fi
else
    log_warn "clang-tidy not found; skipping static analysis"
fi

#--------------------------------------------------------------------------------------------------
# 3. Warnings as errors
#
# Built into a separate directory so it cannot invalidate the main build's object files, which
# would force every later stage to recompile.
#--------------------------------------------------------------------------------------------------
strict_build_dir="${BUILD_DIR}-strict"
log_info "rebuilding with -Werror into ${strict_build_dir}"

if BUILD_DIR="${strict_build_dir}" "${SCRIPT_DIR}/build.sh" \
        -DMOVIEBACKEND_WARNINGS_AS_ERRORS=ON \
        > "${REPORT_DIR}/warnings-as-errors.log" 2>&1; then
    log_ok "no compiler warnings"
else
    log_error "the build produces warnings - see ${REPORT_DIR}/warnings-as-errors.log"
    grep -E 'warning:|error:' "${REPORT_DIR}/warnings-as-errors.log" | head -30 >&2 || true
    failures=$((failures + 1))
fi

#--------------------------------------------------------------------------------------------------
if [ "${failures}" -gt 0 ]; then
    log_error "${failures} quality check(s) failed"
    exit 1
fi

log_ok "all quality checks passed"
