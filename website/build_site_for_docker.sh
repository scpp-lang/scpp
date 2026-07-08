#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SITE_OUTPUT_DIR="${SITE_OUTPUT_DIR:-${REPO_ROOT}/site-dist}"
DOCS_BUILD_SCRIPT="${DOCS_BUILD_SCRIPT:-docs/site/build.sh}"
DOCS_BUILD_OUTPUT_DIR="${DOCS_BUILD_OUTPUT_DIR:-docs/site/dist}"

if [[ "${DOCS_BUILD_SCRIPT}" = /* ]]; then
    build_script_path="${DOCS_BUILD_SCRIPT}"
else
    build_script_path="${REPO_ROOT}/${DOCS_BUILD_SCRIPT}"
fi
if [[ ! -f "${build_script_path}" ]]; then
    echo "error: docs build script not found: ${DOCS_BUILD_SCRIPT}" >&2
    exit 1
fi

if [[ "${DOCS_BUILD_OUTPUT_DIR}" = /* ]]; then
    docs_output_path="${DOCS_BUILD_OUTPUT_DIR}"
else
    docs_output_path="${REPO_ROOT}/${DOCS_BUILD_OUTPUT_DIR}"
fi

bash "${build_script_path}"

if [[ ! -d "${docs_output_path}" ]]; then
    echo "error: docs output directory does not exist after build: ${DOCS_BUILD_OUTPUT_DIR}" >&2
    exit 1
fi

rm -rf "${SITE_OUTPUT_DIR}"
mkdir -p "${SITE_OUTPUT_DIR}"
cp -a "${docs_output_path}/." "${SITE_OUTPUT_DIR}/"
echo "Copied docs site from ${docs_output_path} to ${SITE_OUTPUT_DIR}"
