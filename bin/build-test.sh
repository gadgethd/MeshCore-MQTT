#!/usr/bin/env bash
set -u
set -o pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
BUILD_SCRIPT="${SCRIPT_DIR}/build-meshcore-mqtt-observer.sh"
RESULTS_ROOT="${REPO_ROOT}/.build-test"
RUN_ID=$(date +%Y%m%d-%H%M%S)
RESULTS_DIR="${BUILD_TEST_RESULTS_DIR:-${RESULTS_ROOT}/${RUN_ID}}"
KEEP_ARTIFACTS="${BUILD_TEST_KEEP_ARTIFACTS:-0}"

mkdir -p "${RESULTS_DIR}"

if [ ! -x "${BUILD_SCRIPT}" ]; then
  echo "Build script not found or not executable: ${BUILD_SCRIPT}" >&2
  exit 1
fi

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

slugify() {
  printf '%s' "$1" | tr -c '[:alnum:]_.-' '_'
}

dynamic_build_env() {
  printf '%s' "$1" | tr -c '[:alnum:]_' '_'
  printf '%s' "_mqtt_dynamic"
}

cleanup_build_artifacts() {
  local base_env=$1
  local build_env
  build_env=$(dynamic_build_env "${base_env}")

  rm -rf "${REPO_ROOT}/.pio/build/${build_env}"
}

run_build() {
  local base_env=$1

  (
    if [ -z "${PLATFORMIO_CORE_DIR:-}" ]; then
      export PLATFORMIO_CORE_DIR=/tmp/pio-core
    fi
    export MESHCORE_MQTT_ADVERT_NAME="${MESHCORE_MQTT_ADVERT_NAME:-MeshCore MQTT Build Test}"
    export MESHCORE_MQTT_ADMIN_PASSWORD="${MESHCORE_MQTT_ADMIN_PASSWORD:-password}"
    export MESHCORE_MQTT_BASE_ENV="${base_env}"
    export MESHCORE_MQTT_WIFI_SSID="${MESHCORE_MQTT_WIFI_SSID:-build-test-ssid}"
    export MESHCORE_MQTT_WIFI_PWD="${MESHCORE_MQTT_WIFI_PWD:-build-test-password}"
    export MESHCORE_MQTT_TOPIC_ROOT="${MESHCORE_MQTT_TOPIC_ROOT:-meshcore}"
    export MESHCORE_MQTT_URI="${MESHCORE_MQTT_URI:-wss://example.invalid:443/}"
    export MESHCORE_MQTT_USERNAME="${MESHCORE_MQTT_USERNAME:-build-test-user}"
    export MESHCORE_MQTT_PASSWORD="${MESHCORE_MQTT_PASSWORD:-build-test-password}"
    export MESHCORE_MQTT_IATA="${MESHCORE_MQTT_IATA:-TST}"
    export MESHCORE_MQTT_MODEL="${MESHCORE_MQTT_MODEL:-${base_env}}"
    export MESHCORE_MQTT_CLIENT_VERSION="${MESHCORE_MQTT_CLIENT_VERSION:-custom-mqtt-observer/build-test}"
    cd "${REPO_ROOT}"
    "${BUILD_SCRIPT}" </dev/null
  )
}

if [ "$#" -gt 0 ]; then
  ENVS=("$@")
else
  mapfile -t ENVS < <(discover_esp_repeater_envs)
fi

if [ "${#ENVS[@]}" -eq 0 ]; then
  echo "No ESP repeater envs found." >&2
  exit 1
fi

PASS_ENVS=()
FAIL_ENVS=()

echo "Results directory: ${RESULTS_DIR}"
echo

for env_name in "${ENVS[@]}"; do
  log_file="${RESULTS_DIR}/$(slugify "${env_name}").log"
  printf 'Building %-40s ' "${env_name}"
  echo
  if run_build "${env_name}" 2>&1 | tee "${log_file}"; then
    echo "PASS"
    PASS_ENVS+=("${env_name}")
  else
    echo "FAIL"
    FAIL_ENVS+=("${env_name}")
  fi
  if [ "${KEEP_ARTIFACTS}" != "1" ]; then
    cleanup_build_artifacts "${env_name}"
  fi
done

{
  echo "Results directory: ${RESULTS_DIR}"
  echo "Artifacts kept: ${KEEP_ARTIFACTS}"
  echo "Total: ${#ENVS[@]}"
  echo "Passed: ${#PASS_ENVS[@]}"
  for env_name in "${PASS_ENVS[@]}"; do
    echo "PASS ${env_name}"
  done
  echo "Failed: ${#FAIL_ENVS[@]}"
  for env_name in "${FAIL_ENVS[@]}"; do
    echo "FAIL ${env_name}"
  done
} | tee "${RESULTS_DIR}/summary.txt"

if [ "${#FAIL_ENVS[@]}" -gt 0 ]; then
  exit 1
fi
