#!/bin/bash
# Shared ESP-ADF / ESP-IDF environment bootstrap for all firmware projects.
#
# Do NOT use $ADF_PATH/export.sh when system python3 is 3.13+ — it picks the wrong
# ESP-IDF venv. Use:
#   source scripts/activate_adf_env.sh
#
# Optional override for voice only (only if playback and voice must diverge):
#   export VOICE_ADF_PATH=/path/to/other/esp-adf
#   source scripts/activate_adf_env.sh voice

_adf_resolve_path() {
    local role="${1:-default}"
    local candidate=""

    if [[ "${role}" == "voice" && -n "${VOICE_ADF_PATH:-}" ]]; then
        candidate="${VOICE_ADF_PATH}"
    elif [[ -n "${ADF_PATH:-}" ]]; then
        candidate="${ADF_PATH}"
    elif [[ -f "${HOME}/Projects/esp-adf/export.sh" ]]; then
        candidate="${HOME}/Projects/esp-adf"
    else
        echo "ESP-ADF not found. Set ADF_PATH to your esp-adf checkout." >&2
        return 1
    fi

    if [[ ! -f "${candidate}/export.sh" ]]; then
        echo "Invalid ADF path (${role}): ${candidate}/export.sh not found" >&2
        return 1
    fi

    printf '%s\n' "${candidate}"
}

_esp_python_ok() {
    local py="${1:-}"
    [[ -n "${py}" && -x "${py}" ]] && "${py}" -c "import click" >/dev/null 2>&1
}

_find_idf_venv_python() {
    local candidate=""
    local best=""

    if _esp_python_ok "${HOME}/.espressif/python_env/idf5.4_py3.9_env/bin/python"; then
        printf '%s\n' "${HOME}/.espressif/python_env/idf5.4_py3.9_env/bin/python"
        return 0
    fi

    if [[ -n "${ZSH_VERSION:-}" ]]; then
        setopt local_options null_glob
    fi

    for candidate in "${HOME}"/.espressif/python_env/*/bin/python; do
        if _esp_python_ok "${candidate}"; then
            best="${candidate}"
            break
        fi
    done

    if [[ -n "${best}" ]]; then
        printf '%s\n' "${best}"
    fi
}

source_adf_env() {
    local role="${1:-default}"
    local adf_root
    local shell_name="bash"
    local idf_exports=""

    adf_root="$(_adf_resolve_path "${role}")" || return 1
    export ADF_PATH="${adf_root}"
    export IDF_PATH="${ADF_PATH}/esp-idf"

    if [[ "${role}" == "voice" && -n "${VOICE_ADF_PATH:-}" ]]; then
        echo "Using VOICE_ADF_PATH=${VOICE_ADF_PATH}" >&2
    fi

    if [[ -n "${ZSH_VERSION:-}" ]]; then
        shell_name="zsh"
    fi

    if ! _esp_python_ok "${ESP_PYTHON:-}"; then
        ESP_PYTHON="$(_find_idf_venv_python || true)"
    fi

    if ! _esp_python_ok "${ESP_PYTHON:-}"; then
        echo "No usable ESP-IDF Python venv found under ~/.espressif/python_env." >&2
        echo "Install one with a supported Python (3.9–3.12), for example:" >&2
        echo "  cd \"${IDF_PATH}\" && python3.10 ./install.sh" >&2
        return 1
    fi

    export ESP_PYTHON
    echo "Using ESP-IDF Python: ${ESP_PYTHON}" >&2

    if ! idf_exports="$("${ESP_PYTHON}" "${IDF_PATH}/tools/activate.py" --export --shell "${shell_name}")"; then
        echo "ESP-IDF activate.py failed." >&2
        return 1
    fi
    eval "${idf_exports}"

    if ! "${ESP_PYTHON}" "${ADF_PATH}/tools/adf_install_patches.py" apply-patch; then
        echo "ADF patch install failed." >&2
        return 1
    fi

    return 0
}
