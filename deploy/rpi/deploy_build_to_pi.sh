#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ENV_FILE="${ENV_FILE:-${REPO_ROOT}/.env}"

if [[ -f "${ENV_FILE}" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "${ENV_FILE}"
  set +a
fi

PI_HOST="${PI_HOST:-172.24.217.51}"
PI_USER="${PI_USER:-quarcs}"
PI_PASSWORD="${PI_PASSWORD:-quarcs}"
LOCAL_BUILD_DIR="${LOCAL_BUILD_DIR:-${REPO_ROOT}/build-rpi}"
REMOTE_BUILD_DIR="${REMOTE_BUILD_DIR:-/home/quarcs/workspace/QUARCS/QUARCS_QT-SeverProgram/src/BUILD}"
QUARCS_TOTAL_VERSION="${QUARCS_TOTAL_VERSION:-$(date +%Y%m%d%H%M)}"

ARTIFACTS=(
  client
  guiding_offline_test
  qhyccd.ini
)

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_cmd rsync
require_cmd ssh
require_cmd sshpass

if [[ ! -d "${LOCAL_BUILD_DIR}" ]]; then
  echo "Local build directory not found: ${LOCAL_BUILD_DIR}" >&2
  exit 1
fi

for artifact in "${ARTIFACTS[@]}"; do
  if [[ ! -e "${LOCAL_BUILD_DIR}/${artifact}" ]]; then
    echo "Missing artifact: ${LOCAL_BUILD_DIR}/${artifact}" >&2
    exit 1
  fi
done

SSH_OPTS=(
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o ConnectTimeout=5
)

SSH_CMD=(sshpass -p "${PI_PASSWORD}" ssh "${SSH_OPTS[@]}")
RSYNC_RSH="sshpass -p ${PI_PASSWORD} ssh ${SSH_OPTS[*]}"

echo "Creating remote directory: ${PI_USER}@${PI_HOST}:${REMOTE_BUILD_DIR}"
"${SSH_CMD[@]}" "${PI_USER}@${PI_HOST}" "mkdir -p '${REMOTE_BUILD_DIR}'"

echo "Syncing artifacts from ${LOCAL_BUILD_DIR} to ${PI_USER}@${PI_HOST}:${REMOTE_BUILD_DIR}"
for artifact in "${ARTIFACTS[@]}"; do
  rsync -av --progress -e "${RSYNC_RSH}" \
    "${LOCAL_BUILD_DIR}/${artifact}" \
    "${PI_USER}@${PI_HOST}:${REMOTE_BUILD_DIR}/"
done

echo "Remote files:"
"${SSH_CMD[@]}" "${PI_USER}@${PI_HOST}" "ls -lh '${REMOTE_BUILD_DIR}'/client '${REMOTE_BUILD_DIR}'/guiding_offline_test '${REMOTE_BUILD_DIR}'/qhyccd.ini"

echo "Configured QUARCS_TOTAL_VERSION=${QUARCS_TOTAL_VERSION}"
