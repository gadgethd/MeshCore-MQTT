#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
LOCAL_PIO_VENV="${REPO_ROOT}/.venv-platformio"
BUILD_DIR="${REPO_ROOT}/.pio/build/Heltec_v3_repeater_mqtt"
APP_BIN="${BUILD_DIR}/firmware.bin"
MERGED_BIN="${BUILD_DIR}/firmware-merged.bin"
UPDATE_BIN="${BUILD_DIR}/firmware-update.bin"
FULL_BIN="${BUILD_DIR}/firmware-full.bin"

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

if [ -z "${PLATFORMIO_CORE_DIR:-}" ] && { [ ! -d "${HOME}/.platformio" ] || [ ! -w "${HOME}/.platformio" ]; }; then
  export PLATFORMIO_CORE_DIR=/tmp/pio-core
fi

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

prompt_value MESHCORE_MQTT_ADVERT_NAME "Observer name" "Heltec MQTT"
prompt_value MESHCORE_MQTT_ADMIN_PASSWORD "Admin password" "password"
prompt_value MESHCORE_MQTT_WIFI_SSID "WiFi SSID" "${MESHCORE_MQTT_WIFI_SSID:-}" 0 1
prompt_value MESHCORE_MQTT_WIFI_PWD "WiFi password" "${MESHCORE_MQTT_WIFI_PWD:-}" 1 1
prompt_value MESHCORE_MQTT_TOPIC_ROOT "MQTT topic root" "meshcore" 0 1
prompt_value MESHCORE_MQTT_URI "MQTT WebSocket URI" "wss://mqtt.ukmesh.com:443/" 0 1
prompt_value MESHCORE_MQTT_USERNAME "MQTT username" "${MESHCORE_MQTT_USERNAME:-}" 0 1
prompt_value MESHCORE_MQTT_PASSWORD "MQTT password" "${MESHCORE_MQTT_PASSWORD:-}" 1 1
prompt_value MESHCORE_MQTT_IATA "Observer IATA code" "${MESHCORE_MQTT_IATA:-}" 0 1
prompt_value MESHCORE_MQTT_MODEL "Model label" "Heltec V3"
prompt_value MESHCORE_MQTT_CLIENT_VERSION "Client version" "custom-mqtt-observer/1.0.0"
echo
echo "Building Heltec_v3_repeater_mqtt with:"
echo "  Observer name:    ${MESHCORE_MQTT_ADVERT_NAME}"
echo "  Admin password:   ${MESHCORE_MQTT_ADMIN_PASSWORD}"
echo "  WiFi SSID:        ${MESHCORE_MQTT_WIFI_SSID}"
echo "  WiFi password:    ${MESHCORE_MQTT_WIFI_PWD}"
echo "  MQTT topic root:  ${MESHCORE_MQTT_TOPIC_ROOT}"
echo "  MQTT URI:         ${MESHCORE_MQTT_URI}"
echo "  MQTT username:    ${MESHCORE_MQTT_USERNAME}"
echo "  MQTT password:    ${MESHCORE_MQTT_PASSWORD}"
echo "  Observer IATA:    ${MESHCORE_MQTT_IATA}"
echo "  Model label:      ${MESHCORE_MQTT_MODEL}"
echo "  Client version:   ${MESHCORE_MQTT_CLIENT_VERSION}"
echo "  LoRa radio:       configurable at runtime via MeshCore CLI (default: 869.525 MHz / BW 62.5 / SF8)"
if [ -n "${PLATFORMIO_CORE_DIR:-}" ]; then
  echo "  PLATFORMIO_CORE_DIR: ${PLATFORMIO_CORE_DIR}"
fi
echo

cd "${REPO_ROOT}"
"${PIO_BIN}" run -e Heltec_v3_repeater_mqtt "$@"
MERGED_BIN_PATH="${MERGED_BIN}" "${PIO_BIN}" run -e Heltec_v3_repeater_mqtt -t mergebin "$@"
cp -f "${APP_BIN}" "${UPDATE_BIN}"
cp -f "${MERGED_BIN}" "${FULL_BIN}"

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
