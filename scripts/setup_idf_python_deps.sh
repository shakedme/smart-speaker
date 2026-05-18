#!/bin/bash
# Pin Python packages required by cspot/nanopb code generation in the ESP-IDF venv.
set -euo pipefail

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "Source ESP-ADF first: source ~/Projects/esp-adf/export.sh" >&2
  exit 1
fi

PYTHON="$(command -v python3)"
echo "Installing nanopb build deps into: $PYTHON"
"$PYTHON" -m pip install 'grpcio-tools==1.60.0' 'protobuf>=3.19,<5' 'setuptools==75.8.2'
