#!/usr/bin/env bash
set -euo pipefail

# CI build script for MQTT repeater firmwares.
# Discovers all ESP32-based repeater envs and builds MQTT-enabled firmware for each.
# Usage: ./bin/ci-build-mqtt-repeater.sh

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
OUT_DIR="${REPO_ROOT}/out"
PIO_BIN=""

for candidate in "${REPO_ROOT}/.venv-platformio/bin/pio" "$(command -v pio 2>/dev/null)" "${HOME}/.local/bin/pio"; do
  if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then
    PIO_BIN="${candidate}"
    break
  fi
done

if [ -z "${PIO_BIN}" ]; then
  echo "PlatformIO not found." >&2
  exit 1
fi

if [ -z "${PLATFORMIO_CORE_DIR:-}" ]; then
  export PLATFORMIO_CORE_DIR=/tmp/pio-core
fi

discover_esp_repeater_envs() {
  python3 - "${REPO_ROOT}" <<'PY'
import re
import sys
from pathlib import Path

repo_root = Path(sys.argv[1])
env_re = re.compile(r'^\[env:([^\]]+)\]$')
esp_base_re = re.compile(r'^\s*extends\s*=\s*esp32_base\s*$')
repeater_re = re.compile(r'(^|_)repeater_?$')

for ini_path in sorted((repo_root / "variants").glob("*/platformio.ini")):
    text = ini_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    if not any(esp_base_re.match(line.strip()) for line in text):
        continue
    for line in text:
        match = env_re.match(line.strip())
        if not match:
            continue
        env_name = match.group(1)
        if repeater_re.search(env_name):
            print(env_name)
PY
}

build_mqtt_firmware() {
  local base_env="$1"
  local version="${FIRMWARE_VERSION:-unknown}"
  local build_env
  build_env="$(printf '%s' "${base_env}" | tr -c '[:alnum:]_' '_')_mqtt_ci"

  local temp_conf
  temp_conf=$(mktemp /tmp/meshcore-mqtt-ci-XXXXXX.ini)
  cat > "${temp_conf}" <<EOF
[platformio]
extra_configs =
  ${REPO_ROOT}/platformio.ini

[env:${build_env}]
extends = env:${base_env}
extra_scripts =
  \${env:${base_env}.extra_scripts}
  pre:arch/esp32/extra_scripts/mqtt_build_vars.py
build_flags =
  \${env:${base_env}.build_flags}
  -D WITH_MQTT_REPORTER=1
  -D AUTO_OFF_MILLIS=20000
  -D DISABLE_WIFI_OTA=1
EOF

  echo "  Building ${base_env} (env: ${build_env})..."
  if ! "${PIO_BIN}" run -c "${temp_conf}" -e "${build_env}" > /dev/null 2>&1; then
    echo "  FAILED: ${base_env} compile error"
    rm -f "${temp_conf}"
    return 1
  fi

  # Run mergebin to get the full flash image
  if ! "${PIO_BIN}" run -c "${temp_conf}" -e "${build_env}" -t mergebin > /dev/null 2>&1; then
    echo "  FAILED: ${base_env} mergebin error"
    rm -f "${temp_conf}"
    return 1
  fi

  local firmware_name="meshcore-mqtt-${version}-${base_env}"
  local build_subdir=".pio/build/${build_env}"

  # Copy artifacts to out/
  mkdir -p "${OUT_DIR}/${base_env}"

  if [ -f "${build_subdir}/firmware.bin" ]; then
    cp "${build_subdir}/firmware.bin" "${OUT_DIR}/${base_env}/${firmware_name}-update.bin"
  fi
  if [ -f "${build_subdir}/firmware-merged.bin" ]; then
    cp "${build_subdir}/firmware-merged.bin" "${OUT_DIR}/${base_env}/${firmware_name}-full.bin"
  fi
  if [ -f "${build_subdir}/bootloader.bin" ]; then
    cp "${build_subdir}/bootloader.bin" "${OUT_DIR}/${base_env}/${firmware_name}-bootloader.bin"
  fi
  if [ -f "${build_subdir}/partitions.bin" ]; then
    cp "${build_subdir}/partitions.bin" "${OUT_DIR}/${base_env}/${firmware_name}-partitions.bin"
  fi

  echo "  OK: ${base_env}"
  rm -f "${temp_conf}"
  return 0
}

# --- main ---
echo "Discovering ESP32 repeater envs..."
mapfile -t ENVS < <(discover_esp_repeater_envs)

if [ "${#ENVS[@]}" -eq 0 ]; then
  echo "No ESP32 repeater envs found."
  exit 1
fi

echo "Found ${#ENVS[@]} ESP repeater targets"
echo "Firmware version: ${FIRMWARE_VERSION:-unknown}"
echo

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

PASS=()
FAIL=()

for env in "${ENVS[@]}"; do
  if build_mqtt_firmware "${env}"; then
    PASS+=("${env}")
  else
    FAIL+=("${env}")
  fi
done

echo
echo "=========================================="
echo "Build summary"
echo "=========================================="
echo "Passed: ${#PASS[@]}"
for e in "${PASS[@]}"; do
  echo "  ${e}"
done
echo "Failed: ${#FAIL[@]}"
for e in "${FAIL[@]}"; do
  echo "  ${e}"
done

if [ "${#FAIL[@]}" -gt 0 ]; then
  echo "Some targets failed (C6/C3 experimental boards are expected to fail)."
  echo "Release will include only passing targets."
fi

if [ "${#PASS[@]}" -eq 0 ]; then
  echo "ERROR: No targets built successfully."
  exit 1
fi

echo
echo "Artifacts in: ${OUT_DIR}"
