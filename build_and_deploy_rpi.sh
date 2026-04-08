#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-rpi}"
TOOLCHAIN_FILE="${TOOLCHAIN_FILE:-${REPO_ROOT}/toolchain-rpi-arm64.cmake}"
DEPLOY_SCRIPT="${DEPLOY_SCRIPT:-${REPO_ROOT}/deploy/rpi/deploy_build_to_pi.sh}"
ENV_FILE="${ENV_FILE:-${REPO_ROOT}/.env}"
JOBS="${JOBS:-$(nproc)}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"

if [[ -f "${ENV_FILE}" ]]; then
  set -a
  # shellcheck disable=SC1090
  source "${ENV_FILE}"
  set +a
fi

log() {
  printf '[%s] %s\n' "$(date '+%F %T')" "$*"
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_cmd cmake
require_cmd nproc
require_cmd grep

if [[ ! -d "${REPO_ROOT}/src" ]]; then
  echo "Repository src directory not found: ${REPO_ROOT}/src" >&2
  exit 1
fi

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
  echo "Toolchain file not found: ${TOOLCHAIN_FILE}" >&2
  exit 1
fi

if [[ ! -x "${DEPLOY_SCRIPT}" ]]; then
  echo "Deploy script is missing or not executable: ${DEPLOY_SCRIPT}" >&2
  echo "Try: chmod +x ${DEPLOY_SCRIPT}" >&2
  exit 1
fi

cache_invalid=0
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  if ! grep -Fq "CMAKE_TOOLCHAIN_FILE:FILEPATH=${TOOLCHAIN_FILE}" "${BUILD_DIR}/CMakeCache.txt"; then
    cache_invalid=1
  fi
fi

if [[ "${CLEAN_BUILD}" == "1" || "${cache_invalid}" == "1" ]]; then
  log "Removing build directory: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

log "Configuring cross build"
cmake -S "${REPO_ROOT}/src" -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"

log "Building with ${JOBS} parallel jobs"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

log "Verifying target architecture"
file "${BUILD_DIR}/client" "${BUILD_DIR}/guiding_offline_test"

log "Deploying artifacts to Raspberry Pi"
LOCAL_BUILD_DIR="${BUILD_DIR}" "${DEPLOY_SCRIPT}"

log "Build and deploy completed"
