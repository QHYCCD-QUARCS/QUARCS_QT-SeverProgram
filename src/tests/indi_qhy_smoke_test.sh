#!/usr/bin/env bash
set -euo pipefail

HOST="${INDI_HOST:-127.0.0.1}"
PORT="${INDI_PORT:-7624}"
FIFO_PATH="${INDI_FIFO_PATH:-/tmp/myFIFO}"
DRIVER_NAME="${INDI_QHY_DRIVER:-indi_qhy_ccd}"
START_DRIVER="${INDI_START_DRIVER:-1}"

EXPECT_MODELS=("$@")
if (( ${#EXPECT_MODELS[@]} == 0 )); then
  EXPECT_MODELS=("QHY247C" "QHY5III585")
fi

if ! command -v indi_getprop >/dev/null 2>&1; then
  echo "indi_getprop not found" >&2
  exit 2
fi

if ! command -v indi_setprop >/dev/null 2>&1; then
  echo "indi_setprop not found" >&2
  exit 2
fi

start_driver_if_needed() {
  if [[ "${START_DRIVER}" != "1" ]]; then
    return 0
  fi

  if [[ ! -p "${FIFO_PATH}" ]]; then
    echo "FIFO not found: ${FIFO_PATH}" >&2
    return 1
  fi

  python3 - <<PY
import os
fifo = ${FIFO_PATH@Q}
driver = ${DRIVER_NAME@Q}
fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
os.write(fd, f"start {driver}\n".encode("utf-8"))
os.close(fd)
PY
}

read_qhy_devices() {
  indi_getprop -h "${HOST}" -p "${PORT}" 2>/dev/null \
    | awk -F= '/^QHY CCD .*\.DRIVER_INFO\.DRIVER_EXEC=/{sub(/\.DRIVER_INFO\.DRIVER_EXEC$/, "", $1); print $1}'
}

wait_for_devices() {
  local deadline=$((SECONDS + 20))
  while (( SECONDS < deadline )); do
    mapfile -t DEVICES < <(read_qhy_devices | sort -u)
    if (( ${#DEVICES[@]} > 0 )); then
      return 0
    fi
    sleep 1
  done
  return 1
}

connect_and_verify_device() {
  local device="$1"
  indi_setprop -h "${HOST}" -p "${PORT}" "${device}.CONNECTION.CONNECT=On" >/dev/null

  local deadline=$((SECONDS + 25))
  while (( SECONDS < deadline )); do
    local conn
    conn="$(indi_getprop -h "${HOST}" -p "${PORT}" "${device}.CONNECTION.CONNECT" 2>/dev/null || true)"
    if [[ "${conn}" == *"=On" ]]; then
      break
    fi
    sleep 1
  done

  local info
  info="$(indi_getprop -h "${HOST}" -p "${PORT}" "${device}.CCD_INFO.*" 2>/dev/null || true)"
  if [[ -z "${info}" ]]; then
    echo "FAILED ${device}: CCD_INFO missing after connect" >&2
    return 1
  fi

  echo "CONNECTED ${device}"
  echo "${info}" | sed 's/^/  /'
}

start_driver_if_needed

if ! wait_for_devices; then
  echo "No QHY INDI devices published on ${HOST}:${PORT}" >&2
  exit 1
fi

printf 'Published devices on %s:%s\n' "${HOST}" "${PORT}"
printf '  %s\n' "${DEVICES[@]}"

for expected in "${EXPECT_MODELS[@]}"; do
  if ! printf '%s\n' "${DEVICES[@]}" | grep -Fq "${expected}"; then
    echo "Missing expected model: ${expected}" >&2
    exit 1
  fi
done

for device in "${DEVICES[@]}"; do
  connect_and_verify_device "${device}"
done
