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

GIT_COMMIT=$(git -C "${REPO_ROOT}" rev-parse --short=12 HEAD 2>/dev/null || printf '%s' "unknown")

discover_esp_repeater_envs() {
  python3 - "${REPO_ROOT}" <<'PY'
import re
import sys
from pathlib import Path

repo_root = Path(sys.argv[1])
env_re = re.compile(r'^\[env:([^\]]+)\]$')
esp_base_re = re.compile(r'^\s*extends\s*=\s*(esp32_base|esp32c6_base)\s*$')
repeater_re = re.compile(r'(^|_)repeater_?$')
board_re = re.compile(r'^\s*board\s*=\s*([^\s;]+)')

for ini_path in sorted((repo_root / "variants").glob("*/platformio.ini")):
    text = ini_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    if not any(esp_base_re.match(line.strip()) for line in text):
        continue
    board_match = next((board_re.match(line) for line in text if board_re.match(line)), None)
    board = board_match.group(1).lower() if board_match else ""
    for line in text:
        match = env_re.match(line.strip())
        if not match:
            continue
        env_name = match.group(1)
        if repeater_re.search(env_name):
            target = f"{ini_path.parent.name} {board} {env_name}".lower()
            if "c3" in target or "c6" in target:
                print(f"Skipping unsupported RISC-V MQTT target: {env_name}", file=sys.stderr)
                continue
            print(env_name)
PY
}

# Resolve an environment's effective board and flash size.
# OTA is enabled only when the resolved size is at least 16 MB.  Unknown or
# malformed sizes deliberately fall back to disabled.
get_board_flash_info() {
  local env_name="$1"

  python3 - "${REPO_ROOT}" "${PLATFORMIO_CORE_DIR}" "${env_name}" <<'PY'
import json
import re
import sys
from pathlib import Path

repo_root = Path(sys.argv[1])
platformio_core_dir = Path(sys.argv[2])
env_name = sys.argv[3]


def strip_inline_comment(value):
    return value.split(";", 1)[0].strip()


def read_ini(path):
    sections = {}
    current = None
    for raw_line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw_line.strip()
        if not line or line.startswith((";", "#")):
            continue
        section_match = re.match(r"^\[([^\]]+)\]$", line)
        if section_match:
            current = section_match.group(1).strip()
            sections.setdefault(current, {})
            continue
        if current is None or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip().lower()
        if key in {"board", "board_upload.flash_size", "extends"}:
            sections[current][key] = strip_inline_comment(value)
    return sections


def find_section(sections, name):
    candidates = [name]
    if name.startswith("env:"):
        candidates.append(name[4:])
    else:
        candidates.append(f"env:{name}")
    return next((candidate for candidate in candidates if candidate in sections), None)


def resolve_section(sections, name, stack=None):
    stack = set() if stack is None else stack
    section_name = find_section(sections, name)
    if section_name is None or section_name in stack:
        return {}

    section = sections[section_name]
    resolved = {}
    next_stack = stack | {section_name}
    for parent in re.split(r"\s*,\s*", section.get("extends", "")):
        parent = parent.strip()
        if parent:
            resolved.update(resolve_section(sections, parent, next_stack))
    resolved.update({key: value for key, value in section.items() if key != "extends"})
    return resolved


def leading_mb(value):
    match = re.match(r"^\s*(\d+)", str(value))
    return match.group(1) if match else None


def json_flash_size(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return leading_mb(data.get("upload", {}).get("flash_size", ""))
    except (OSError, ValueError, AttributeError):
        return None


variant_path = None
sections = None
for candidate in sorted((repo_root / "variants").glob("*/platformio.ini")):
    candidate_sections = read_ini(candidate)
    if f"env:{env_name}" in candidate_sections:
        variant_path = candidate
        sections = candidate_sections
        break

if variant_path is None:
    print("-\tunknown\tunknown")
    raise SystemExit

effective = resolve_section(sections, f"env:{env_name}")
board = effective.get("board", "").strip()
if not board:
    board = next(
        (values.get("board", "").strip() for values in sections.values() if values.get("board")),
        "",
    )

if not board:
    print("-\tunknown\tunknown")
    raise SystemExit

variant_mb = leading_mb(effective.get("board_upload.flash_size", ""))
if variant_mb is not None:
    print(f"{board.lower()}\t{variant_mb}\tvariant_override")
    raise SystemExit

repo_board = repo_root / "boards" / f"{board}.json"
repo_mb = json_flash_size(repo_board) if repo_board.is_file() else None
if repo_mb is not None:
    print(f"{board.lower()}\t{repo_mb}\trepo_board_json")
    raise SystemExit

platformio_board = (
    platformio_core_dir
    / "platforms"
    / "espressif32"
    / "boards"
    / f"{board}.json"
)
platformio_mb = json_flash_size(platformio_board) if platformio_board.is_file() else None
if platformio_mb is not None:
    print(f"{board.lower()}\t{platformio_mb}\tplatformio_board_json")
    raise SystemExit

print(f"{board.lower()}\tunknown\tunknown")
PY
}

run_pio() {
  if [ "${MQTT_CI_VERBOSE:-0}" = "1" ]; then
    "${PIO_BIN}" "$@"
  else
    "${PIO_BIN}" "$@" > /dev/null 2>&1
  fi
}

run_mqtt_pio() {
  local ota_env_value="$1"
  shift

  if [ "${ota_env_value}" = "1" ]; then
    MESHCORE_MQTT_ENABLE_OTA=1 run_pio "$@"
  else
    MESHCORE_MQTT_ENABLE_OTA=0 run_pio "$@"
  fi
}

build_mqtt_firmware() {
  local base_env="$1"
  local version="${FIRMWARE_VERSION:-unknown}"
  local build_env
  build_env="$(printf '%s' "${base_env}" | tr -c '[:alnum:]_' '_')_mqtt_ci"

  local ota_enabled="${OTA_ENABLED[${base_env}]:-0}"
  local ota_flag="  -D DISABLE_WIFI_OTA=1"
  local ota_env_value=0
  if [ "${ota_enabled}" = "1" ]; then
    ota_flag="  -D ENABLE_WIFI_OTA=1"
    ota_env_value=1
  fi

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
EOF

  if [ "${ota_enabled}" = "1" ]; then
    cat >> "${temp_conf}" <<EOF
build_unflags =
  \${env:${base_env}.build_unflags}
  -D DISABLE_WIFI_OTA=1
EOF
  fi

  cat >> "${temp_conf}" <<EOF
build_flags =
  \${env:${base_env}.build_flags}
  -D WITH_MQTT_REPORTER=1
  -D AUTO_OFF_MILLIS=20000
${ota_flag}
  -D MESHCORE_GIT_COMMIT=\"${GIT_COMMIT}\"
EOF

  if [ "${ota_enabled}" = "1" ]; then
    cat >> "${temp_conf}" <<EOF
lib_deps =
  \${env:${base_env}.lib_deps}
  \${esp32_ota.lib_deps}
EOF
  fi

  echo "  Building ${base_env} (env: ${build_env}; flash ${FLASH_MB[${base_env}]} MB; OTA $([ "${ota_enabled}" = "1" ] && printf enabled || printf disabled))..."
  if ! run_mqtt_pio "${ota_env_value}" run -c "${temp_conf}" -e "${build_env}"; then
    echo "  FAILED: ${base_env} compile error"
    rm -f "${temp_conf}"
    return 1
  fi

  # Run mergebin to get the full flash image
  if ! run_mqtt_pio "${ota_env_value}" run -c "${temp_conf}" -e "${build_env}" -t mergebin; then
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
  # boot_app0 (OTA data initializer, offset 0xe000) is not emitted into the
  # build dir by PlatformIO; ship the framework-bundled copy, which is
  # byte-identical to the image embedded in the merged full build, so the
  # flasher importer always receives a complete segment set.
  local boot_app0_src="${build_subdir}/boot_app0.bin"
  if [ ! -f "${boot_app0_src}" ]; then
    boot_app0_src="$(find "${PLATFORMIO_CORE_DIR:-${HOME}/.platformio}/packages" -maxdepth 5 -path '*framework-arduinoespressif32*/tools/partitions/boot_app0.bin' 2>/dev/null | head -n1)"
  fi
  if [ -n "${boot_app0_src}" ] && [ -f "${boot_app0_src}" ]; then
    cp "${boot_app0_src}" "${OUT_DIR}/${base_env}/${firmware_name}-boot_app0.bin"
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
# Stamp the client version from the build's firmware version (tag releases pass
# FIRMWARE_VERSION, e.g. v1.17.1) so published metadata can never lag a release.
export MESHCORE_MQTT_CLIENT_VERSION="${MESHCORE_MQTT_CLIENT_VERSION:-meshcore-mqtt/${FIRMWARE_VERSION:-unknown}}"
echo

declare -A BOARD_NAME FLASH_MB FLASH_SOURCE OTA_ENABLED
for env in "${ENVS[@]}"; do
  IFS=$'\t' read -r BOARD_NAME["${env}"] FLASH_MB["${env}"] FLASH_SOURCE["${env}"] < <(get_board_flash_info "${env}")
  if [[ "${FLASH_MB[${env}]}" =~ ^[0-9]+$ ]] && (( FLASH_MB[${env}] >= 16 )); then
    OTA_ENABLED["${env}"]=1
  else
    OTA_ENABLED["${env}"]=0
  fi
done

echo "OTA decision table"
echo "env | board | flash MB | OTA enabled/disabled (source of size)"
for env in "${ENVS[@]}"; do
  local_decision=disabled
  if [ "${OTA_ENABLED[${env}]}" = "1" ]; then
    local_decision=enabled
  fi
  printf '%s | %s | %s | %s (%s)\n' \
    "${env}" "${BOARD_NAME[${env}]}" "${FLASH_MB[${env}]}" \
    "${local_decision}" "${FLASH_SOURCE[${env}]}"
done

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

PASS=()
FAIL=()

BUILD_ENVS=("${ENVS[@]}")
if [ -n "${MQTT_CI_ONLY:-}" ]; then
  selected_env="${MQTT_CI_ONLY}"
  selected_base=""
  for env in "${ENVS[@]}"; do
    if [ "${env}" = "${selected_env}" ]; then
      selected_base="${env}"
      break
    fi
  done
  if [ -z "${selected_base}" ]; then
    # Convenience alias: MQTT_CI_ONLY=foo_repeater_mqtt selects the generated
    # MQTT build based on the existing foo_repeater environment.
    mqtt_base="${selected_env%_mqtt}"
    for env in "${ENVS[@]}"; do
      if [ "${env}" = "${mqtt_base}" ]; then
        selected_base="${env}"
        break
      fi
    done
  fi
  if [ -z "${selected_base}" ]; then
    echo "MQTT_CI_ONLY did not match a discovered repeater env: ${selected_env}" >&2
    exit 1
  fi
  BUILD_ENVS=("${selected_base}")
  echo
  echo "Build filter: ${selected_env} (generated from ${selected_base})"
fi

for env in "${BUILD_ENVS[@]}"; do
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
  echo "ERROR: supported MQTT targets failed to build."
  exit 1
fi

if [ "${#PASS[@]}" -eq 0 ]; then
  echo "ERROR: No targets built successfully."
  exit 1
fi

echo
echo "Artifacts in: ${OUT_DIR}"
