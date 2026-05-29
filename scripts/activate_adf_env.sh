#!/usr/bin/env bash
# One-shot ESP-ADF / ESP-IDF activation (safe with system python3 3.13+).
#
# From repo root:
#   source scripts/activate_adf_env.sh
#
# Voice ADF override:
#   source scripts/activate_adf_env.sh voice

set -euo pipefail

if [[ -n "${ZSH_VERSION:-}" ]]; then
    _SCRIPT_DIR="$(cd "$(dirname "${(%):-%x}")" && pwd)"
else
    _SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
# shellcheck source=scripts/source_adf_env.sh
source "${_SCRIPT_DIR}/source_adf_env.sh"
source_adf_env "${1:-default}"
