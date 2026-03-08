#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
LOCAL_PIO_VENV="${REPO_ROOT}/.venv-platformio"
BASE_ENV_DEFAULT="Heltec_v3_repeater"
BUILD_ENV=""
BUILD_DIR=""
APP_BIN=""
MERGED_BIN=""
UPDATE_BIN=""
FULL_BIN=""
TEMP_CONF=""
WEBFLASHER_DIR=""
WEBFLASHER_PREFIX=""
WEBFLASHER_UPDATE_BIN=""
WEBFLASHER_FULL_BIN=""

if [ -x "${LOCAL_PIO_VENV}/bin/pio" ]; then
  PIO_BIN="${LOCAL_PIO_VENV}/bin/pio"
elif command -v pio >/dev/null 2>&1; then
  PIO_BIN=$(command -v pio)
elif [ -x "${HOME}/.local/bin/pio" ]; then
  PIO_BIN="${HOME}/.local/bin/pio"
else
  echo "PlatformIO was not found. Create ${LOCAL_PIO_VENV} with Python 3.12/3.13 or set PIO_BIN." >&2
  exit 1
fi

if [ -z "${PLATFORMIO_CORE_DIR:-}" ]; then
  if [ "${PIO_BIN}" = "${LOCAL_PIO_VENV}/bin/pio" ] || { [ ! -d "${HOME}/.platformio" ] || [ ! -w "${HOME}/.platformio" ]; }; then
    export PLATFORMIO_CORE_DIR=/tmp/pio-core
  fi
fi

cleanup() {
  if [ -n "${TEMP_CONF}" ] && [ -f "${TEMP_CONF}" ]; then
    rm -f "${TEMP_CONF}"
  fi
}

trap cleanup EXIT

discover_esp_repeater_envs() {
  python3 - "${REPO_ROOT}" <<'PY'
import re
import sys
from pathlib import Path

repo_root = Path(sys.argv[1])
env_re = re.compile(r'^\[env:([^\]]+)\]$')
esp_base_re = re.compile(r'^\s*extends\s*=\s*(esp32_base|esp32c6_base)\s*$')
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

sanitize_name() {
  printf '%s' "$1" | tr -c '[:alnum:]._+-' '-'
}

extract_firmware_version() {
  sed -n 's/^  #define FIRMWARE_VERSION   "\(.*\)"/\1/p' "${REPO_ROOT}/examples/simple_repeater/MyMesh.h" | head -n 1
}

build_dynamic_env() {
  local base_env=$1
  local build_env=$2

  TEMP_CONF=$(mktemp /tmp/meshcore-mqtt-build-XXXXXX.ini)
  cat > "${TEMP_CONF}" <<EOF
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
  -D AUTO_OFF_MILLIS=0
EOF
}

prompt_value() {
  local var_name=$1
  local prompt=$2
  local default_value=$3
  local secret=${4:-0}
  local required=${5:-0}
  local current_value=${!var_name:-$default_value}
  local input=""

  while true; do
    if [ ! -t 0 ]; then
      input=${current_value}
      if [ "${required}" = "1" ] && [ -z "${input}" ]; then
        echo "Missing required value for ${var_name} in non-interactive mode." >&2
        exit 1
      fi
      printf -v "${var_name}" "%s" "${input}"
      export "${var_name}"
      return
    fi

    if [ -n "${current_value}" ]; then
      printf "%s [%s]: " "${prompt}" "${current_value}"
    else
      printf "%s: " "${prompt}"
    fi

    if [ "${secret}" = "1" ]; then
      read -r -s input
      printf "\n"
    else
      read -r input
    fi

    if [ -z "${input}" ]; then
      input=${current_value}
    fi

    if [ "${required}" = "1" ] && [ -z "${input}" ]; then
      echo "This value is required."
      continue
    fi

    printf -v "${var_name}" "%s" "${input}"
    export "${var_name}"
    return
  done
}

prompt_value MESHCORE_MQTT_ADVERT_NAME "Observer name" "MeshCore MQTT"
prompt_value MESHCORE_MQTT_ADMIN_PASSWORD "Admin password" "password"
prompt_value MESHCORE_MQTT_BASE_ENV "Base ESP repeater env" "${MESHCORE_MQTT_BASE_ENV:-$BASE_ENV_DEFAULT}" 0 1
prompt_value MESHCORE_MQTT_WIFI_SSID "WiFi SSID default" "${MESHCORE_MQTT_WIFI_SSID:-}"
prompt_value MESHCORE_MQTT_WIFI_PWD "WiFi password default" "${MESHCORE_MQTT_WIFI_PWD:-}" 1
prompt_value MESHCORE_MQTT_TOPIC_ROOT "MQTT topic root default" "meshcore"
prompt_value MESHCORE_MQTT_URI "MQTT WebSocket URI default" "${MESHCORE_MQTT_URI:-}"
prompt_value MESHCORE_MQTT_USERNAME "MQTT username default" "${MESHCORE_MQTT_USERNAME:-}"
prompt_value MESHCORE_MQTT_PASSWORD "MQTT password default" "${MESHCORE_MQTT_PASSWORD:-}" 1
prompt_value MESHCORE_MQTT_IATA "Observer IATA code default" "${MESHCORE_MQTT_IATA:-XXX}"
prompt_value MESHCORE_MQTT_MODEL "Model label" "${MESHCORE_MQTT_MODEL:-$MESHCORE_MQTT_BASE_ENV}"
prompt_value MESHCORE_MQTT_CLIENT_VERSION "Client version" "custom-mqtt-observer/1.0.0"

AVAILABLE_ENVS=$(discover_esp_repeater_envs)
if ! printf '%s\n' "${AVAILABLE_ENVS}" | grep -Fxq "${MESHCORE_MQTT_BASE_ENV}"; then
  echo "Unknown ESP repeater env: ${MESHCORE_MQTT_BASE_ENV}" >&2
  echo "Available ESP repeater envs:" >&2
  printf '  %s\n' ${AVAILABLE_ENVS} >&2
  exit 1
fi

BUILD_ENV="$(printf '%s' "${MESHCORE_MQTT_BASE_ENV}" | tr -c '[:alnum:]_' '_')_mqtt_dynamic"
BUILD_DIR="${REPO_ROOT}/.pio/build/${BUILD_ENV}"
APP_BIN="${BUILD_DIR}/firmware.bin"
MERGED_BIN="${BUILD_DIR}/firmware-merged.bin"
UPDATE_BIN="${BUILD_DIR}/firmware-update.bin"
FULL_BIN="${BUILD_DIR}/firmware-full.bin"
WEBFLASHER_DIR="${REPO_ROOT}/webflasher/${MESHCORE_MQTT_BASE_ENV}"
FIRMWARE_VERSION_NAME="$(sanitize_name "$(extract_firmware_version)")"
if [ -z "${FIRMWARE_VERSION_NAME}" ]; then
  FIRMWARE_VERSION_NAME="unknown"
fi
WEBFLASHER_PREFIX="meshcore-mqtt-${FIRMWARE_VERSION_NAME}-${MESHCORE_MQTT_BASE_ENV}"
WEBFLASHER_UPDATE_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-update.bin"
WEBFLASHER_FULL_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-full.bin"
build_dynamic_env "${MESHCORE_MQTT_BASE_ENV}" "${BUILD_ENV}"

echo
echo "Building ${BUILD_ENV} from ${MESHCORE_MQTT_BASE_ENV} with:"
echo "  Observer name:    ${MESHCORE_MQTT_ADVERT_NAME}"
echo "  Admin password:   ${MESHCORE_MQTT_ADMIN_PASSWORD}"
echo "  Base env:         ${MESHCORE_MQTT_BASE_ENV}"
echo "  WiFi SSID:        ${MESHCORE_MQTT_WIFI_SSID:-<set later via serial>}"
echo "  WiFi password:    ${MESHCORE_MQTT_WIFI_PWD:-<set later via serial>}"
echo "  MQTT topic root:  ${MESHCORE_MQTT_TOPIC_ROOT:-meshcore}"
echo "  MQTT URI:         ${MESHCORE_MQTT_URI:-<set later via serial>}"
echo "  MQTT username:    ${MESHCORE_MQTT_USERNAME:-<set later via serial>}"
echo "  MQTT password:    ${MESHCORE_MQTT_PASSWORD:-<set later via serial>}"
echo "  Observer IATA:    ${MESHCORE_MQTT_IATA:-XXX}"
echo "  Model label:      ${MESHCORE_MQTT_MODEL}"
echo "  Client version:   ${MESHCORE_MQTT_CLIENT_VERSION}"
echo "  LoRa radio:       configurable at runtime via MeshCore CLI (default: 869.525 MHz / BW 62.5 / SF8)"
if [ -n "${PLATFORMIO_CORE_DIR:-}" ]; then
  echo "  PLATFORMIO_CORE_DIR: ${PLATFORMIO_CORE_DIR}"
fi
echo

cd "${REPO_ROOT}"
"${PIO_BIN}" run -c "${TEMP_CONF}" -e "${BUILD_ENV}" "$@"
MERGED_BIN_PATH="${MERGED_BIN}" "${PIO_BIN}" run -c "${TEMP_CONF}" -e "${BUILD_ENV}" -t mergebin "$@"
cp -f "${APP_BIN}" "${UPDATE_BIN}"
cp -f "${MERGED_BIN}" "${FULL_BIN}"

mkdir -p "${WEBFLASHER_DIR}"
rm -f "${WEBFLASHER_DIR}"/*.bin "${WEBFLASHER_DIR}/manifest.json"
cp -f "${UPDATE_BIN}" "${WEBFLASHER_UPDATE_BIN}"
cp -f "${FULL_BIN}" "${WEBFLASHER_FULL_BIN}"
cat > "${WEBFLASHER_DIR}/manifest.json" <<EOF
{
  "base_env": "${MESHCORE_MQTT_BASE_ENV}",
  "build_env": "${BUILD_ENV}",
  "firmware_name": "meshcore-mqtt",
  "firmware_version": "${FIRMWARE_VERSION_NAME}",
  "built_at_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "artifacts": {
    "update": "$(basename "${WEBFLASHER_UPDATE_BIN}")",
    "full": "$(basename "${WEBFLASHER_FULL_BIN}")"
  },
  "flash_offsets": {
    "update": "0x10000",
    "full": "0x00000"
  }
}
EOF

echo
echo "Built firmware images:"
echo "  App image: ${APP_BIN}"
echo "  Update image: ${UPDATE_BIN}"
echo "  Full flash image: ${MERGED_BIN}"
echo "  Full flash alias: ${FULL_BIN}"
echo
echo "Flash usage:"
echo "  Update/app image -> flash at 0x10000"
echo "  Full flash image -> flash at 0x00000"
echo
echo "Webflasher output:"
echo "  ${WEBFLASHER_DIR}"
echo "  $(basename "${WEBFLASHER_UPDATE_BIN}")"
echo "  $(basename "${WEBFLASHER_FULL_BIN}")"
