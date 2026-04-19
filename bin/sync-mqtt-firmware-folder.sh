#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
BRANCH="${1:-dev}"

if [ "${BRANCH}" != "dev" ] && [ "${BRANCH}" != "main" ]; then
  echo "Usage: bin/sync-mqtt-firmware-folder.sh [dev|main]" >&2
  exit 1
fi

SOURCE_DIR="${REPO_ROOT}/webflasher/${BRANCH}"
DEST_DIR="${REPO_ROOT}/MQTT_Firmware/${BRANCH}"

if [ ! -d "${SOURCE_DIR}" ]; then
  echo "Firmware source directory does not exist: ${SOURCE_DIR}" >&2
  exit 1
fi

mkdir -p "${REPO_ROOT}/MQTT_Firmware"
rm -rf "${DEST_DIR}"
mkdir -p "${DEST_DIR}"

cp -a "${SOURCE_DIR}/." "${DEST_DIR}/"

echo "Synced MQTT firmware:"
echo "  from: ${SOURCE_DIR}"
echo "  to:   ${DEST_DIR}"
