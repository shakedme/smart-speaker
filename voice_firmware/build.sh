#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DIR="${SCRIPT_DIR}"

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1090
    source "${REPO_ROOT}/scripts/source_adf_env.sh"
    source_adf_env voice
fi

export IDF_CCACHE_ENABLE="${IDF_CCACHE_ENABLE:-1}"

idf.py -C "${PROJECT_DIR}" "$@"
