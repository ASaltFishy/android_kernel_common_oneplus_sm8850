#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build/push the camera Scudo preload artifacts and run the phone-side overlay
# recovery script. The phone-side work is done from a copied script instead of a
# deeply nested adb shell command.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"
MODULE_DIR="${SCRIPT_DIR}/../kernelsu/modules/md-camera-scudo"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_UPDATE_DIR="${PHONE_UPDATE_DIR:-/data/local/tmp/md-camera-scudo-update}"
BUILD="${BUILD:-1}"
export ADB_SERVER_SOCKET

STAMP="$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="${REMOTE_DIR:-/tmp/md-camera-scudo-update-${STAMP}}"
PHONE_SCRIPT="${SCRIPT_DIR}/recover_camera_scudo_overlay_phone.sh"
JUMP_SCRIPT="${SCRIPT_DIR}/recover_camera_scudo_overlay_jump.sh"

LIB_SCUDO="${TESTS_DIR}/out/libscudo_md_android.so"
LIB_PRELOAD="${TESTS_DIR}/out/libscudo_md_preload.so"
BROKER="${TESTS_DIR}/out/memory_delegation_broker"
WRAPPER="${MODULE_DIR}/system/bin/md_camera_scudo_wrap.sh"
SERVICE="${MODULE_DIR}/service.sh"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android \
    libscudo_md_android_so libscudo_md_preload_so memory_delegation_broker
fi

for f in "${LIB_SCUDO}" "${LIB_PRELOAD}" "${BROKER}" "${WRAPPER}" "${SERVICE}" "${PHONE_SCRIPT}" "${JUMP_SCRIPT}"; do
  [[ -f "${f}" ]] || {
    echo "missing required file: ${f}" >&2
    exit 1
  }
done

mkdir -p "${REMOTE_DIR}"
cp -p "${LIB_SCUDO}" "${REMOTE_DIR}/libscudo_md_android.so"
cp -p "${LIB_PRELOAD}" "${REMOTE_DIR}/libscudo_md_preload.so"
cp -p "${BROKER}" "${REMOTE_DIR}/memory_delegation_broker"
cp -p "${WRAPPER}" "${REMOTE_DIR}/md_camera_scudo_wrap.sh"
cp -p "${SERVICE}" "${REMOTE_DIR}/service.sh"
cp -p "${PHONE_SCRIPT}" "${REMOTE_DIR}/recover_camera_scudo_overlay_phone.sh"
export ADB_BIN ADB_SERIAL REMOTE_DIR PHONE_UPDATE_DIR
bash "${JUMP_SCRIPT}"
