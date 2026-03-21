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
BOOTLOADER_BIN=""
PARTITIONS_BIN=""
BOOT_APP0_BIN=""
TEMP_CONF=""
WEBFLASHER_DIR=""
WEBFLASHER_PREFIX=""
WEBFLASHER_UPDATE_BIN=""
WEBFLASHER_FULL_BIN=""
WEBFLASHER_BOOTLOADER_BIN=""
WEBFLASHER_PARTITIONS_BIN=""
WEBFLASHER_BOOT_APP0_BIN=""
WEBFLASHER_SITE_ROOT="${WEBFLASHER_SITE_ROOT:-${HOME}/MeshCore-MQTT-WebFlasher}"
WEBFLASHER_SITE_FIRMWARE_DIR="${WEBFLASHER_SITE_ROOT}/firmware"
WEBFLASHER_SITE_COMPOSE="${WEBFLASHER_SITE_ROOT}/compose.yml"
WEBFLASHER_AUTO_DEPLOY="${WEBFLASHER_AUTO_DEPLOY:-1}"
FLASH_SEGMENTS_JSON=""

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

extract_flash_segments() {
  python3 -c '
import json
import re
import sys

text = sys.stdin.read()
segments = []
for offset, path in re.findall(r"(0x[0-9a-fA-F]+)\s+(\S+\.bin)", text):
    segments.append({"offset": offset.lower(), "path": path})

required_names = {"bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"}
found_names = {segment["path"].rsplit("/", 1)[-1] for segment in segments}
missing = sorted(required_names - found_names)
if missing:
    raise SystemExit(f"Missing merge_bin segments: {'"'"', '"'"'.join(missing)}")

print(json.dumps(segments))
'
}

generate_main_firmware_data() {
  local main_dir="${REPO_ROOT}/webflasher"
  local assets_dir="${main_dir}/assets"
  local output_file="${assets_dir}/firmware-data.js"

  if [ ! -d "${main_dir}" ]; then
    echo "Main firmware directory not found: ${main_dir}"
    return 1
  fi

  # Ensure assets directory exists
  mkdir -p "${assets_dir}"

  echo "Generating firmware-data.js for main branch..."

  # Start the JS file
  cat > "${output_file}" <<EOF
window.FIRMWARE_DATA = {
  "generatedAt": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "branch": "main",
  "boards": [
EOF

  local first=true
  for board_dir in "${main_dir}"/*/; do
    if [ ! -d "${board_dir}" ] || [ "$(basename "${board_dir}")" = "assets" ]; then
      continue
    fi

    board_name=$(basename "${board_dir}")
    manifest="${board_dir}manifest.json"

    if [ ! -f "${manifest}" ]; then
      continue
    fi

    # Extract data from manifest
    firmware_version=$(python3 -c "import json; print(json.load(open('${manifest}'))['firmware_version'])" 2>/dev/null || echo "unknown")
    base_env=$(python3 -c "import json; print(json.load(open('${manifest}'))['base_env'])" 2>/dev/null || echo "${board_name}")

    # Get artifact filenames
    update_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['update'])" 2>/dev/null || echo "")
    full_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['full'])" 2>/dev/null || echo "")
    bootloader_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['bootloader'])" 2>/dev/null || echo "")
    partitions_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['partitions'])" 2>/dev/null || echo "")
    boot_app0_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['boot_app0'])" 2>/dev/null || echo "")

    # Determine chip family from board name heuristics
    chip_family="ESP32"
    case "${board_name}" in
      *S3*|*s3*|*C3*|*c3*|*C6*|*c6*)
        chip_family="ESP32-S3"
        ;;
      *S2*|*s2*)
        chip_family="ESP32-S2"
        ;;
    esac

    # Create label from board name
    label=$(echo "${board_name}" | sed 's/_repeater$//' | sed 's/_/ /g' | sed 's/  */ /g')

    if [ "${first}" = true ]; then
      first=false
    else
      echo "," >> "${output_file}"
    fi

    # Include all artifacts for main
    cat >> "${output_file}" <<EOF
    {
      "id": "${board_name}",
      "label": "${label}",
      "firmwareName": "meshcore-mqtt",
      "firmwareVersion": "${firmware_version}",
      "chipFamily": "${chip_family}",
      "hardwareStatus": "Verified",
      "manifestPath": "/firmware/${board_name}/manifest.json",
      "artifactBase": "/firmware/${board_name}/",
      "artifacts": {
        "full": "${full_bin}",
        "update": "${update_bin}",
        "bootloader": "${bootloader_bin}",
        "partitions": "${partitions_bin}",
        "boot_app0": "${boot_app0_bin}"
      }
    }
EOF
  done

  cat >> "${output_file}" <<EOF
  ]
};
EOF

  echo "Generated: ${output_file}"
}

generate_dev_firmware_data() {
  local dev_dir="${REPO_ROOT}/webflasher/dev"
  local assets_dir="${REPO_ROOT}/webflasher/assets"
  local output_file="${assets_dir}/firmware-data-dev.js"

  if [ ! -d "${dev_dir}" ]; then
    echo "Dev firmware directory not found: ${dev_dir}"
    return 1
  fi

  # Ensure assets directory exists
  mkdir -p "${assets_dir}"

  echo "Generating firmware-data-dev.js..."

  # Start the JS file
  cat > "${output_file}" <<EOF
window.FIRMWARE_DATA = {
  "generatedAt": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "branch": "dev",
  "boards": [
EOF

  local first=true
  for board_dir in "${dev_dir}"/*/; do
    if [ ! -d "${board_dir}" ]; then
      continue
    fi

    board_name=$(basename "${board_dir}")
    manifest="${board_dir}manifest.json"

    if [ ! -f "${manifest}" ]; then
      continue
    fi

    # Extract data from manifest
    firmware_version=$(python3 -c "import json; print(json.load(open('${manifest}'))['firmware_version'])" 2>/dev/null || echo "dev")
    base_env=$(python3 -c "import json; print(json.load(open('${manifest}'))['base_env'])" 2>/dev/null || echo "${board_name}")

    # Get artifact filenames
    update_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['update'])" 2>/dev/null || echo "")
    full_bin=$(python3 -c "import json; print(json.load(open('${manifest}'))['artifacts']['full'])" 2>/dev/null || echo "")

    # Determine chip family from board name heuristics
    case "${board_name}" in
      *S3*|*s3*|*C3*|*c3*|*C6*|*c6*)
        chip_family="ESP32-S3"
        ;;
      *S2*|*s2*)
        chip_family="ESP32-S2"
        ;;
      *)
        chipFamily="ESP32"
        ;;
    esac

    # Create label from board name
    label=$(echo "${board_name}" | sed 's/_/ /g' | sed 's/  */ /g')

    if [ "${first}" = true ]; then
      first=false
    else
      echo "," >> "${output_file}"
    fi

    cat >> "${output_file}" <<EOF
    {
      "id": "${board_name}",
      "label": "${label}",
      "firmwareName": "meshcore-mqtt",
      "firmwareVersion": "${firmware_version}-dev",
      "chipFamily": "${chipFamily:-ESP32}",
      "hardwareStatus": "Dev build",
      "manifestPath": "/firmware/dev/${board_name}/manifest.json",
      "artifactBase": "/firmware/dev/${board_name}/",
      "artifacts": {
        "full": "${full_bin}",
        "update": "${update_bin}"
      }
    }
EOF
  done

  cat >> "${output_file}" <<EOF
  ]
};
EOF

  echo "Generated: ${output_file}"
}

sync_webflasher_site() {
  if [ "${WEBFLASHER_AUTO_DEPLOY}" = "0" ]; then
    return 0
  fi

  if [ ! -d "${WEBFLASHER_SITE_ROOT}" ]; then
    echo "Skipping web flasher deploy: ${WEBFLASHER_SITE_ROOT} not found." >&2
    return 0
  fi

  if ! command -v rsync >/dev/null 2>&1; then
    echo "rsync is required to sync web flasher firmware output." >&2
    return 1
  fi

  mkdir -p "${WEBFLASHER_SITE_FIRMWARE_DIR}"
  rsync -a --delete "${REPO_ROOT}/webflasher/" "${WEBFLASHER_SITE_FIRMWARE_DIR}/"

  if [ ! -f "${WEBFLASHER_SITE_COMPOSE}" ]; then
    echo "Skipping web flasher rebuild: ${WEBFLASHER_SITE_COMPOSE} not found." >&2
    return 0
  fi

  if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required to rebuild the web flasher container." >&2
    return 1
  fi

  echo
  echo "Rebuilding web flasher container..."
  docker compose -f "${WEBFLASHER_SITE_COMPOSE}" up -d --build flasher-site
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
  -D AUTO_OFF_MILLIS=20000
  -D DISABLE_WIFI_OTA=1
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

# Firmware branch selection - skip if already set by parent script
if [ -n "${BRANCH:-}" ]; then
  MESHCORE_MQTT_BRANCH="${BRANCH}"
  BRANCH_SELECTED_BY_PARENT=1
  echo "Using branch from environment: ${MESHCORE_MQTT_BRANCH}"
else
  BRANCH_SELECTED_BY_PARENT=0
  echo
  echo "=========================================="
  echo "Firmware Branch Selection"
  echo "=========================================="
  echo "Choose which branch to build:"
  echo "  1) Main (Stable) - Production firmware"
  echo "  2) Dev (Unstable) - Development/testing firmware"
  echo

  BRANCH_CHOICE=""
  while [ -z "${BRANCH_CHOICE}" ]; do
    if [ ! -t 0 ]; then
      BRANCH_CHOICE="1"
      break
    fi
    printf "Enter choice [1]: "
    read -r choice
    case "${choice}" in
      1|"")
        BRANCH_CHOICE="main"
        ;;
      2)
        BRANCH_CHOICE="dev"
        ;;
      *)
        echo "Invalid choice. Please enter 1 or 2."
        ;;
    esac
  done

  MESHCORE_MQTT_BRANCH="${BRANCH_CHOICE}"
fi

echo "Selected branch: ${MESHCORE_MQTT_BRANCH}"
echo

if [ "${MESHCORE_MQTT_BRANCH}" = "dev" ] && [ "${BRANCH_SELECTED_BY_PARENT}" != "1" ]; then
  echo "WARNING: Building DEVELOPMENT firmware!"
  echo "This is unstable and not for production use."
  echo

  # Check if dev branch exists and switch to it
  if git rev-parse --verify dev >/dev/null 2>&1; then
    current_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
    if [ "${current_branch}" != "dev" ]; then
      echo "Switching to dev branch..."
      git checkout dev
      echo
    fi
  else
    echo "WARNING: dev branch not found locally. Building from current branch."
  fi
fi

prompt_value MESHCORE_MQTT_ADVERT_NAME "Observer name" "MeshCore MQTT"
prompt_value MESHCORE_MQTT_ADMIN_PASSWORD "Admin password" "password"
prompt_value MESHCORE_MQTT_BASE_ENV "Base ESP repeater env" "${MESHCORE_MQTT_BASE_ENV:-$BASE_ENV_DEFAULT}" 0 1
prompt_value MESHCORE_MQTT_WIFI_SSID "WiFi SSID default" "${MESHCORE_MQTT_WIFI_SSID:-}"
prompt_value MESHCORE_MQTT_WIFI_PWD "WiFi password default" "${MESHCORE_MQTT_WIFI_PWD:-}" 1
prompt_value MESHCORE_MQTT_MODEL "Model label" "${MESHCORE_MQTT_MODEL:-$MESHCORE_MQTT_BASE_ENV}"
prompt_value MESHCORE_MQTT_CLIENT_VERSION "Client version" "custom-mqtt-observer/1.0.0"

prompt_value MESHCORE_MQTT_BROKER1_URI "MQTT broker 1 URI default" "${MESHCORE_MQTT_BROKER1_URI:-${MESHCORE_MQTT_URI:-}}"
prompt_value MESHCORE_MQTT_BROKER1_USERNAME "MQTT broker 1 username default" "${MESHCORE_MQTT_BROKER1_USERNAME:-${MESHCORE_MQTT_USERNAME:-}}"
prompt_value MESHCORE_MQTT_BROKER1_PASSWORD "MQTT broker 1 password default" "${MESHCORE_MQTT_BROKER1_PASSWORD:-${MESHCORE_MQTT_PASSWORD:-}}" 1
prompt_value MESHCORE_MQTT_BROKER1_TOPIC_ROOT "MQTT broker 1 topic root default" "${MESHCORE_MQTT_BROKER1_TOPIC_ROOT:-${MESHCORE_MQTT_TOPIC_ROOT:-meshcore}}"
prompt_value MESHCORE_MQTT_BROKER1_IATA "MQTT broker 1 IATA code default" "${MESHCORE_MQTT_BROKER1_IATA:-${MESHCORE_MQTT_IATA:-XXX}}"
prompt_value MESHCORE_MQTT_BROKER1_RETAIN_STATUS "MQTT broker 1 retain status default (0/1)" "${MESHCORE_MQTT_BROKER1_RETAIN_STATUS:-${MESHCORE_MQTT_RETAIN_STATUS:-1}}"
prompt_value MESHCORE_MQTT_BROKER1_ENABLED "MQTT broker 1 enabled default (0/1)" "${MESHCORE_MQTT_BROKER1_ENABLED:-1}"

for idx in 2 3 4 5 6; do
  broker_enabled_var="MESHCORE_MQTT_BROKER${idx}_ENABLED"
  broker_uri_var="MESHCORE_MQTT_BROKER${idx}_URI"
  broker_user_var="MESHCORE_MQTT_BROKER${idx}_USERNAME"
  broker_pass_var="MESHCORE_MQTT_BROKER${idx}_PASSWORD"
  broker_root_var="MESHCORE_MQTT_BROKER${idx}_TOPIC_ROOT"
  broker_iata_var="MESHCORE_MQTT_BROKER${idx}_IATA"
  broker_retain_var="MESHCORE_MQTT_BROKER${idx}_RETAIN_STATUS"

  prompt_value "${broker_enabled_var}" "MQTT broker ${idx} enabled default (0/1)" "${!broker_enabled_var:-0}"
  prompt_value "${broker_uri_var}" "MQTT broker ${idx} URI default" "${!broker_uri_var:-}"
  prompt_value "${broker_user_var}" "MQTT broker ${idx} username default" "${!broker_user_var:-}"
  prompt_value "${broker_pass_var}" "MQTT broker ${idx} password default" "${!broker_pass_var:-}" 1
  prompt_value "${broker_root_var}" "MQTT broker ${idx} topic root default" "${!broker_root_var:-}"
  prompt_value "${broker_iata_var}" "MQTT broker ${idx} IATA code default" "${!broker_iata_var:-}"
  prompt_value "${broker_retain_var}" "MQTT broker ${idx} retain status default (0/1)" "${!broker_retain_var:-0}"
done

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
# Include branch in output directory (main or dev)
WEBFLASHER_DIR="${REPO_ROOT}/webflasher/${MESHCORE_MQTT_BRANCH:-main}/${MESHCORE_MQTT_BASE_ENV}"
FIRMWARE_VERSION_NAME="$(sanitize_name "$(extract_firmware_version)")"
if [ -z "${FIRMWARE_VERSION_NAME}" ]; then
  FIRMWARE_VERSION_NAME="unknown"
fi
WEBFLASHER_PREFIX="meshcore-mqtt-${FIRMWARE_VERSION_NAME}-${MESHCORE_MQTT_BASE_ENV}"
WEBFLASHER_UPDATE_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-update.bin"
WEBFLASHER_FULL_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-full.bin"
WEBFLASHER_BOOTLOADER_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-bootloader.bin"
WEBFLASHER_PARTITIONS_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-partitions.bin"
WEBFLASHER_BOOT_APP0_BIN="${WEBFLASHER_DIR}/${WEBFLASHER_PREFIX}-boot_app0.bin"
build_dynamic_env "${MESHCORE_MQTT_BASE_ENV}" "${BUILD_ENV}"

echo
echo "Building ${BUILD_ENV} from ${MESHCORE_MQTT_BASE_ENV} with:"
echo "  Observer name:    ${MESHCORE_MQTT_ADVERT_NAME}"
echo "  Admin password:   ${MESHCORE_MQTT_ADMIN_PASSWORD}"
echo "  Base env:         ${MESHCORE_MQTT_BASE_ENV}"
echo "  WiFi SSID:        ${MESHCORE_MQTT_WIFI_SSID:-<set later via serial>}"
echo "  WiFi password:    ${MESHCORE_MQTT_WIFI_PWD:-<set later via serial>}"
echo "  Model label:      ${MESHCORE_MQTT_MODEL}"
echo "  Client version:   ${MESHCORE_MQTT_CLIENT_VERSION}"
for idx in 1 2 3 4 5 6; do
  broker_enabled_var="MESHCORE_MQTT_BROKER${idx}_ENABLED"
  broker_uri_var="MESHCORE_MQTT_BROKER${idx}_URI"
  broker_user_var="MESHCORE_MQTT_BROKER${idx}_USERNAME"
  broker_pass_var="MESHCORE_MQTT_BROKER${idx}_PASSWORD"
  broker_root_var="MESHCORE_MQTT_BROKER${idx}_TOPIC_ROOT"
  broker_iata_var="MESHCORE_MQTT_BROKER${idx}_IATA"
  broker_retain_var="MESHCORE_MQTT_BROKER${idx}_RETAIN_STATUS"
  echo "  Broker ${idx}:        enabled=${!broker_enabled_var:-0} uri=${!broker_uri_var:-<set later via serial>} user=${!broker_user_var:-<set later via serial>} pass=${!broker_pass_var:-<set later via serial>} topic.root=${!broker_root_var:-<default>} iata=${!broker_iata_var:-<default>} retain=${!broker_retain_var:-0}"
done
echo "  LoRa radio:       configurable at runtime via MeshCore CLI (default: 869.525 MHz / BW 62.5 / SF8)"
if [ -n "${PLATFORMIO_CORE_DIR:-}" ]; then
  echo "  PLATFORMIO_CORE_DIR: ${PLATFORMIO_CORE_DIR}"
fi
echo

cd "${REPO_ROOT}"
"${PIO_BIN}" run -c "${TEMP_CONF}" -e "${BUILD_ENV}" "$@"
MERGE_OUTPUT=$(
  MERGED_BIN_PATH="${MERGED_BIN}" "${PIO_BIN}" run -c "${TEMP_CONF}" -e "${BUILD_ENV}" -t mergebin "$@" 2>&1
)
printf '%s\n' "${MERGE_OUTPUT}"
FLASH_SEGMENTS_JSON="$(printf '%s\n' "${MERGE_OUTPUT}" | extract_flash_segments)"
BOOTLOADER_BIN="$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["path"] for segment in segments if segment["path"].endswith("bootloader.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
PARTITIONS_BIN="$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["path"] for segment in segments if segment["path"].endswith("partitions.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
BOOT_APP0_BIN="$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["path"] for segment in segments if segment["path"].endswith("boot_app0.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
cp -f "${APP_BIN}" "${UPDATE_BIN}"
cp -f "${MERGED_BIN}" "${FULL_BIN}"

mkdir -p "${WEBFLASHER_DIR}"
rm -f "${WEBFLASHER_DIR}"/*.bin "${WEBFLASHER_DIR}/manifest.json"
cp -f "${UPDATE_BIN}" "${WEBFLASHER_UPDATE_BIN}"
cp -f "${FULL_BIN}" "${WEBFLASHER_FULL_BIN}"
cp -f "${BOOTLOADER_BIN}" "${WEBFLASHER_BOOTLOADER_BIN}"
cp -f "${PARTITIONS_BIN}" "${WEBFLASHER_PARTITIONS_BIN}"
cp -f "${BOOT_APP0_BIN}" "${WEBFLASHER_BOOT_APP0_BIN}"
cat > "${WEBFLASHER_DIR}/manifest.json" <<EOF
{
  "base_env": "${MESHCORE_MQTT_BASE_ENV}",
  "build_env": "${BUILD_ENV}",
  "firmware_name": "meshcore-mqtt",
  "firmware_version": "${FIRMWARE_VERSION_NAME}",
  "built_at_utc": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "artifacts": {
    "update": "$(basename "${WEBFLASHER_UPDATE_BIN}")",
    "full": "$(basename "${WEBFLASHER_FULL_BIN}")",
    "bootloader": "$(basename "${WEBFLASHER_BOOTLOADER_BIN}")",
    "partitions": "$(basename "${WEBFLASHER_PARTITIONS_BIN}")",
    "boot_app0": "$(basename "${WEBFLASHER_BOOT_APP0_BIN}")"
  },
  "flash_offsets": {
    "bootloader": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("bootloader.bin")))' <<<"${FLASH_SEGMENTS_JSON}")",
    "partitions": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("partitions.bin")))' <<<"${FLASH_SEGMENTS_JSON}")",
    "boot_app0": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("boot_app0.bin")))' <<<"${FLASH_SEGMENTS_JSON}")",
    "update": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("firmware.bin")))' <<<"${FLASH_SEGMENTS_JSON}")",
    "full": "0x00000"
  },
  "update_segments": [
    {
      "name": "bootloader",
      "path": "$(basename "${WEBFLASHER_BOOTLOADER_BIN}")",
      "offset": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("bootloader.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
    },
    {
      "name": "partitions",
      "path": "$(basename "${WEBFLASHER_PARTITIONS_BIN}")",
      "offset": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("partitions.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
    },
    {
      "name": "boot_app0",
      "path": "$(basename "${WEBFLASHER_BOOT_APP0_BIN}")",
      "offset": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("boot_app0.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
    },
    {
      "name": "firmware",
      "path": "$(basename "${WEBFLASHER_UPDATE_BIN}")",
      "offset": "$(python3 -c 'import json,sys; segments=json.loads(sys.stdin.read()); print(next(segment["offset"] for segment in segments if segment["path"].endswith("firmware.bin")))' <<<"${FLASH_SEGMENTS_JSON}")"
    }
  ],
  "full_segments": [
    {
      "name": "merged",
      "path": "$(basename "${WEBFLASHER_FULL_BIN}")",
      "offset": "0x00000"
    }
  ]
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
echo "  $(basename "${WEBFLASHER_BOOTLOADER_BIN}")"
echo "  $(basename "${WEBFLASHER_PARTITIONS_BIN}")"
echo "  $(basename "${WEBFLASHER_BOOT_APP0_BIN}")"

# Generate firmware-data.js for the selected branch
if [ "${MESHCORE_MQTT_BRANCH}" = "dev" ]; then
  generate_dev_firmware_data
else
  generate_main_firmware_data
fi

sync_webflasher_site
