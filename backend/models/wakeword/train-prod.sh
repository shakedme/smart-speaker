#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VENV="${ROOT}/.venv"
LW="${VENV}/bin/livekit-wakeword"
CONFIG="${1:-okay_rufus.yaml}"

cd "$(dirname "$0")"

if [[ ! -x "${LW}" ]]; then
  echo "Missing ${LW}. Run: python3.11 -m venv ${VENV} && ${VENV}/bin/pip install -r requirements-train.txt"
  exit 1
fi

echo "==> Setup (shared data + Piper weights)"
"${LW}" setup --config "${CONFIG}"

echo "==> Full pipeline: generate, augment, train, export, eval"
"${LW}" run "${CONFIG}"
"${LW}" export "${CONFIG}"
"${LW}" eval "${CONFIG}"

EVAL="output/okay_rufus/okay_rufus_eval.json"
if [[ -f "${EVAL}" ]]; then
  echo "==> Eval metrics:"
  cat "${EVAL}"
  echo ""
  echo "Set WAKEWORD_THRESHOLD from the 'threshold' field above (or omit it to auto-load from eval)."
fi
