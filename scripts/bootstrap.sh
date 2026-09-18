#!/usr/bin/env bash
# =============================================================================
# Bootstrap build script for scpp
#
# Multi-stage compilation flow:
#   Stage 0 (STAGE=0): Builds Stage-0 scpp using host C++ compiler and compiles
#                      workspace packages into self-hosted archives (.scppa).
#   Stage 1 (STAGE=1): Uses Stage-0 scpp to compile main.cpp, then links
#                      scpp_stage1 against the workspace archives and runs smoke tests.
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Configurable build directories
STAGE0_BUILD_DIR="${STAGE0_BUILD_DIR:-${BUILD_DIR:-${REPO_ROOT}/build}}"
STAGE1_BUILD_DIR="${STAGE1_BUILD_DIR:-${STAGE1_DIR:-${REPO_ROOT}/build-stage1}}"

# Generator detection: prefer Ninja if available
if [[ -z "${CMAKE_GENERATOR:-}" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        CMAKE_GENERATOR="Ninja"
    else
        CMAKE_GENERATOR="Unix Makefiles"
    fi
fi

echo "================================================================================"
echo " Stage 0: Building scpp compiler and workspace packages with host toolchain"
echo " Build directory: ${STAGE0_BUILD_DIR}"
echo " Generator:       ${CMAKE_GENERATOR}"
echo "================================================================================"

STAGE=0 cmake -S "${REPO_ROOT}" -B "${STAGE0_BUILD_DIR}" -G "${CMAKE_GENERATOR}" "$@"
cmake --build "${STAGE0_BUILD_DIR}"

STAGE0_SCPP="${STAGE0_BUILD_DIR}/scpp"
if [[ ! -x "${STAGE0_SCPP}" ]]; then
    echo "Error: Stage 0 compiler not found at ${STAGE0_SCPP}" >&2
    exit 1
fi

echo "================================================================================"
echo " Stage 1: Building self-hosted scpp_stage1 using Stage 0 compiler"
echo " Stage 0 compiler: ${STAGE0_SCPP}"
echo " Build directory:  ${STAGE1_BUILD_DIR}"
echo " Generator:        ${CMAKE_GENERATOR}"
echo "================================================================================"

STAGE=1 cmake -S "${REPO_ROOT}" -B "${STAGE1_BUILD_DIR}" -G "${CMAKE_GENERATOR}" \
    -DSTAGE0_SCPP="${STAGE0_SCPP}" "$@"
cmake --build "${STAGE1_BUILD_DIR}"

echo "================================================================================"
echo " Stage 1 Smoke Tests"
echo "================================================================================"
"${STAGE1_BUILD_DIR}/scpp_stage1" --version
ctest --test-dir "${STAGE1_BUILD_DIR}" --output-on-failure

echo "================================================================================"
echo " Bootstrap build completed successfully!"
echo " Stage 0 binary: ${STAGE0_SCPP}"
echo " Stage 1 binary: ${STAGE1_BUILD_DIR}/scpp_stage1"
echo "================================================================================"
