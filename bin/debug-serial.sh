#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
LOG_DIR="${REPO_ROOT}/serial-logs"
LOCAL_PIO_VENV="${REPO_ROOT}/.venv-platformio"

PORT="${1:-}"
BAUD="${2:-115200}"

# Auto-detect port if not specified
if [ -z "${PORT}" ]; then
  for candidate in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
    if [ -e "${candidate}" ]; then
      PORT="${candidate}"
      break
    fi
  done
fi

if [ -z "${PORT}" ]; then
  echo "No serial port found. Specify one as first argument: $0 /dev/ttyUSB0" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}"
TIMESTAMP=$(date +"%Y-%m-%d_%H-%M-%S")
LOG_FILE="${LOG_DIR}/meshcore_${TIMESTAMP}.log"

echo "Logging serial output from ${PORT} at ${BAUD} baud"
echo "Log file: ${LOG_FILE}"
echo "Press Ctrl+C to stop."
echo "----------------------------------------"

# Use pio device monitor if available, otherwise fall back to python3
if [ -x "${LOCAL_PIO_VENV}/bin/pio" ]; then
  "${LOCAL_PIO_VENV}/bin/pio" device monitor \
    --port "${PORT}" \
    --baud "${BAUD}" \
    --filter time \
    2>&1 | tee "${LOG_FILE}"
else
  python3 - "${PORT}" "${BAUD}" "${LOG_FILE}" <<'PY'
import sys
import serial
import datetime

port, baud, log_file = sys.argv[1], int(sys.argv[2]), sys.argv[3]

with serial.Serial(port, baud, timeout=1) as ser, open(log_file, "w", buffering=1) as f:
    while True:
        try:
            line = ser.readline()
            if not line:
                continue
            ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            decoded = line.decode("utf-8", errors="replace").rstrip()
            out = f"[{ts}] {decoded}"
            print(out)
            f.write(out + "\n")
        except KeyboardInterrupt:
            break
PY
fi
