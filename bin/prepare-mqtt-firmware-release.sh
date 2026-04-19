#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
BUILD_SCRIPT="${SCRIPT_DIR}/build-meshcore-mqtt-observer.sh"

BRANCH="dev"
VERSION=""
CLEAN_BRANCH_OUTPUT=1
STAGE_OUTPUT=1
ALLOW_DIRTY=0
DRY_RUN=0
ENV_ARGS=()

usage() {
  cat <<'EOF'
Usage:
  bin/prepare-mqtt-firmware-release.sh --version VERSION [options] [env ...]

Builds MQTT observer firmware artifacts for one or more ESP repeater envs, writes
version markers, and stages the generated MQTT_Firmware files ready for commit.

Options:
  --version VERSION  Required release marker, for example v1.15.0-mqtt.1
  --branch BRANCH    Firmware output branch marker: dev or main (default: dev)
  --no-clean         Do not remove previous branch firmware output before building
  --no-stage         Build and write markers but do not git add generated files
  --allow-dirty      Allow uncommitted tracked changes while building
  --dry-run          Print the envs and markers that would be used, then exit
  -h, --help         Show this help

If no envs are supplied, every ESP repeater env discovered from variants/*/platformio.ini
is built. Passing env names limits the build, for example:

  bin/prepare-mqtt-firmware-release.sh --version v1.15.0-mqtt.1 Heltec_v3_repeater heltec_v4_repeater

The script does not commit or push. After it finishes, review the staged MQTT_Firmware
files, commit them, then push the branch.

The script automatically switches to the requested branch before building. If a
fork/<branch> remote-tracking branch exists, it fast-forwards the local branch to
that remote when possible.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

sanitize_name() {
  printf '%s' "$1" | tr -c '[:alnum:]._+-' '-'
}

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

json_write_release_metadata() {
  local branch_dir=$1
  local release_file=$2
  shift 2

  python3 - "${branch_dir}" "${release_file}" "$@" <<'PY'
import json
import sys
from pathlib import Path

branch_dir = Path(sys.argv[1])
release_file = Path(sys.argv[2])
metadata = json.loads(sys.argv[3])
envs = sys.argv[4:]

boards = []
for env_name in envs:
    board_dir = branch_dir / env_name
    manifest_path = board_dir / "manifest.json"
    if not manifest_path.exists():
        raise SystemExit(f"missing manifest: {manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest.update(metadata)
    manifest["release_manifest"] = str(release_file.relative_to(branch_dir.parent.parent))
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    artifacts = manifest.get("artifacts", {})
    boards.append(
        {
            "base_env": manifest.get("base_env", env_name),
            "firmware_version": manifest.get("firmware_version"),
            "manifest": str(manifest_path.relative_to(branch_dir.parent.parent)),
            "artifacts": {
                name: str((board_dir / filename).relative_to(branch_dir.parent.parent))
                for name, filename in sorted(artifacts.items())
            },
        }
    )

release_payload = dict(metadata)
release_payload["boards"] = boards
release_file.write_text(json.dumps(release_payload, indent=2) + "\n", encoding="utf-8")
PY
}

prepare_source_branch() {
  local current_branch
  current_branch=$(git rev-parse --abbrev-ref HEAD)

  if git remote get-url fork >/dev/null 2>&1; then
    git fetch fork "${BRANCH}" >/dev/null 2>&1 || true
  fi

  if git show-ref --verify --quiet "refs/heads/${BRANCH}"; then
    if [ "${current_branch}" != "${BRANCH}" ]; then
      echo "Switching from ${current_branch} to ${BRANCH}..."
      git switch "${BRANCH}"
    fi
  elif git show-ref --verify --quiet "refs/remotes/fork/${BRANCH}"; then
    echo "Creating local ${BRANCH} from fork/${BRANCH}..."
    git switch -c "${BRANCH}" --track "fork/${BRANCH}"
  else
    die "branch ${BRANCH} was not found locally or at fork/${BRANCH}"
  fi

  if git show-ref --verify --quiet "refs/remotes/fork/${BRANCH}"; then
    local local_ref
    local local_sha
    local remote_sha
    local_ref=$(git rev-parse "${BRANCH}")
    local_sha=$(git rev-parse "${local_ref}")
    remote_sha=$(git rev-parse "fork/${BRANCH}")

    if [ "${local_sha}" != "${remote_sha}" ]; then
      if git merge-base --is-ancestor "${local_sha}" "${remote_sha}"; then
        echo "Fast-forwarding ${BRANCH} to fork/${BRANCH}..."
        git merge --ff-only "fork/${BRANCH}"
      elif git merge-base --is-ancestor "${remote_sha}" "${local_sha}"; then
        echo "Local ${BRANCH} is ahead of fork/${BRANCH}; using local ${BRANCH}."
      else
        die "local ${BRANCH} and fork/${BRANCH} have diverged; resolve that before building firmware"
      fi
    fi
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      [ "$#" -ge 2 ] || die "--version requires a value"
      VERSION=$2
      shift 2
      ;;
    --version=*)
      VERSION=${1#*=}
      shift
      ;;
    --branch)
      [ "$#" -ge 2 ] || die "--branch requires a value"
      BRANCH=$2
      shift 2
      ;;
    --branch=*)
      BRANCH=${1#*=}
      shift
      ;;
    --no-clean)
      CLEAN_BRANCH_OUTPUT=0
      shift
      ;;
    --no-stage)
      STAGE_OUTPUT=0
      shift
      ;;
    --allow-dirty)
      ALLOW_DIRTY=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [ "$#" -gt 0 ]; do
        ENV_ARGS+=("$1")
        shift
      done
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      ENV_ARGS+=("$1")
      shift
      ;;
  esac
done

[ -n "${VERSION}" ] || die "--version is required"
[ "${BRANCH}" = "dev" ] || [ "${BRANCH}" = "main" ] || die "--branch must be dev or main"
[ -x "${BUILD_SCRIPT}" ] || die "build script is not executable: ${BUILD_SCRIPT}"

cd "${REPO_ROOT}"

if [ "${ALLOW_DIRTY}" != "1" ] && ! git diff --quiet --ignore-submodules --; then
  die "tracked files have unstaged changes; commit/stash them or pass --allow-dirty"
fi

if [ "${ALLOW_DIRTY}" != "1" ] && ! git diff --cached --quiet --ignore-submodules --; then
  die "tracked files have staged changes; commit/stash them or pass --allow-dirty"
fi

prepare_source_branch

[ -x "${BUILD_SCRIPT}" ] || die "build script is not executable: ${BUILD_SCRIPT}"

if [ "${#ENV_ARGS[@]}" -gt 0 ]; then
  ENVS=("${ENV_ARGS[@]}")
else
  mapfile -t ENVS < <(discover_esp_repeater_envs)
fi

[ "${#ENVS[@]}" -gt 0 ] || die "no ESP repeater envs found"

SAFE_VERSION=$(sanitize_name "${VERSION}")
GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
GIT_COMMIT=$(git rev-parse HEAD)
GIT_SHORT=$(git rev-parse --short=12 HEAD)
GIT_DESCRIBE=$(git describe --tags --dirty --always 2>/dev/null || git rev-parse --short HEAD)
BUILT_AT=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
CLIENT_MARKER="meshcore-mqtt/${SAFE_VERSION}+${BRANCH}.${GIT_SHORT}"
BUILD_BRANCH_DIR="${REPO_ROOT}/webflasher/${BRANCH}"
OUTPUT_BRANCH_DIR="${REPO_ROOT}/MQTT_Firmware/${BRANCH}"
RELEASE_FILE="${OUTPUT_BRANCH_DIR}/release.json"
LOG_ROOT="${REPO_ROOT}/.build-test/firmware-release-${SAFE_VERSION}-${BRANCH}-$(date -u +"%Y%m%d-%H%M%S")"

cat <<EOF
Firmware release preparation
  version:       ${VERSION}
  output branch: ${BRANCH}
  git branch:    ${GIT_BRANCH}
  git commit:    ${GIT_COMMIT}
  client marker: ${CLIENT_MARKER}
  env count:     ${#ENVS[@]}
EOF

printf '  envs:\n'
for env_name in "${ENVS[@]}"; do
  printf '    %s\n' "${env_name}"
done

if [ "${DRY_RUN}" = "1" ]; then
  echo
  echo "Dry run only; no firmware was built."
  exit 0
fi

if [ "${CLEAN_BRANCH_OUTPUT}" = "1" ]; then
  rm -rf "${BUILD_BRANCH_DIR}"
  rm -rf "${OUTPUT_BRANCH_DIR}"
fi
mkdir -p "${BUILD_BRANCH_DIR}" "${OUTPUT_BRANCH_DIR}" "${LOG_ROOT}"

PASS_ENVS=()
FAIL_ENVS=()

for env_name in "${ENVS[@]}"; do
  log_file="${LOG_ROOT}/$(sanitize_name "${env_name}").log"
  echo
  echo "Building ${env_name}; log: ${log_file}"

  if (
    export BRANCH="${BRANCH}"
    export WEBFLASHER_AUTO_DEPLOY=0
    export MESHCORE_MQTT_BASE_ENV="${env_name}"
    export MESHCORE_MQTT_MODEL="${MESHCORE_MQTT_MODEL:-${env_name}}"
    export MESHCORE_MQTT_CLIENT_VERSION="${CLIENT_MARKER}"
    export MESHCORE_MQTT_ADVERT_NAME="${MESHCORE_MQTT_ADVERT_NAME:-MeshCore MQTT}"
    export MESHCORE_MQTT_ADMIN_PASSWORD="${MESHCORE_MQTT_ADMIN_PASSWORD:-password}"
    export MESHCORE_MQTT_WIFI_SSID="${MESHCORE_MQTT_WIFI_SSID:-}"
    export MESHCORE_MQTT_WIFI_PWD="${MESHCORE_MQTT_WIFI_PWD:-}"
    export MESHCORE_MQTT_BROKER1_URI="${MESHCORE_MQTT_BROKER1_URI:-${MESHCORE_MQTT_URI:-}}"
    export MESHCORE_MQTT_BROKER1_USERNAME="${MESHCORE_MQTT_BROKER1_USERNAME:-${MESHCORE_MQTT_USERNAME:-}}"
    export MESHCORE_MQTT_BROKER1_PASSWORD="${MESHCORE_MQTT_BROKER1_PASSWORD:-${MESHCORE_MQTT_PASSWORD:-}}"
    export MESHCORE_MQTT_BROKER1_TOPIC_ROOT="${MESHCORE_MQTT_BROKER1_TOPIC_ROOT:-${MESHCORE_MQTT_TOPIC_ROOT:-meshcore}}"
    export MESHCORE_MQTT_BROKER1_IATA="${MESHCORE_MQTT_BROKER1_IATA:-${MESHCORE_MQTT_IATA:-XXX}}"
    export MESHCORE_MQTT_BROKER1_RETAIN_STATUS="${MESHCORE_MQTT_BROKER1_RETAIN_STATUS:-${MESHCORE_MQTT_RETAIN_STATUS:-1}}"
    export MESHCORE_MQTT_BROKER1_ENABLED="${MESHCORE_MQTT_BROKER1_ENABLED:-1}"
    "${BUILD_SCRIPT}" </dev/null
  ) 2>&1 | tee "${log_file}"; then
    PASS_ENVS+=("${env_name}")
  else
    FAIL_ENVS+=("${env_name}")
  fi
done

{
  echo "version=${VERSION}"
  echo "branch=${BRANCH}"
  echo "git_commit=${GIT_COMMIT}"
  echo "client_marker=${CLIENT_MARKER}"
  echo "built_at_utc=${BUILT_AT}"
  echo "passed=${#PASS_ENVS[@]}"
  printf 'PASS %s\n' "${PASS_ENVS[@]}"
  echo "failed=${#FAIL_ENVS[@]}"
  printf 'FAIL %s\n' "${FAIL_ENVS[@]}"
} | tee "${LOG_ROOT}/summary.txt"

if [ "${#FAIL_ENVS[@]}" -gt 0 ]; then
  echo
  echo "One or more firmware builds failed; not writing release markers or staging output." >&2
  exit 1
fi

rm -rf "${OUTPUT_BRANCH_DIR}"
mkdir -p "${OUTPUT_BRANCH_DIR}"
cp -a "${BUILD_BRANCH_DIR}/." "${OUTPUT_BRANCH_DIR}/"

METADATA=$(
  python3 - "${VERSION}" "${BRANCH}" "${GIT_BRANCH}" "${GIT_COMMIT}" "${GIT_DESCRIBE}" "${BUILT_AT}" "${CLIENT_MARKER}" <<'PY'
import json
import sys

keys = [
    "release_version",
    "release_branch",
    "source_git_branch",
    "source_git_commit",
    "source_git_describe",
    "built_at_utc",
    "mqtt_client_version_marker",
]
print(json.dumps(dict(zip(keys, sys.argv[1:]))))
PY
)

json_write_release_metadata "${OUTPUT_BRANCH_DIR}" "${RELEASE_FILE}" "${METADATA}" "${PASS_ENVS[@]}"

if [ "${STAGE_OUTPUT}" = "1" ]; then
  git add "${OUTPUT_BRANCH_DIR}"
fi

echo
echo "Firmware artifacts are ready."
echo "  release output: ${OUTPUT_BRANCH_DIR}"
echo "  build scratch:  ${BUILD_BRANCH_DIR}"
echo "  marker:         ${RELEASE_FILE}"
echo "  logs:           ${LOG_ROOT}"
if [ "${STAGE_OUTPUT}" = "1" ]; then
  echo
  echo "Staged files:"
  git diff --cached --name-status -- "${OUTPUT_BRANCH_DIR}"
fi
