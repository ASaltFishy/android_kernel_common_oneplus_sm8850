#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build/push the Bilibili Scudo preload artifacts and configure only
# wrap.tv.danmaku.bili to a real wrapper script path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"
WRAPPER_DIR="${SCRIPT_DIR}/modules/md-system-scudo/system/bin"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_WORK_DIR="${PHONE_WORK_DIR:-/data/local/tmp/md-bili-scudo}"
PKG="${PKG:-tv.danmaku.bili}"
BUILD="${BUILD:-1}"
BIND_WRAP_TARGET="${BIND_WRAP_TARGET:-0}"
export ADB_SERVER_SOCKET

PHONE_SCRIPT="${SCRIPT_DIR}/recover_bili_scudo_overlay_phone.sh"

LIB_SCUDO="${TESTS_DIR}/out/libscudo_md_preload.so"
BROKER="${TESTS_DIR}/out/memory_delegation_broker"
WRAPPER="${WRAPPER_DIR}/md_bili_scudo_wrap.sh"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android \
    libscudo_md_preload_so memory_delegation_broker
fi

for f in "${LIB_SCUDO}" "${BROKER}" "${WRAPPER}" "${PHONE_SCRIPT}"; do
  [[ -f "${f}" ]] || {
    echo "missing required file: ${f}" >&2
    exit 1
  }
done

adb_cmd=("${ADB_BIN}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

dst="${PHONE_WORK_DIR}"
echo "adb: wait-for-device (ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET})"
"${adb_cmd[@]}" wait-for-device
"${adb_cmd[@]}" shell "su -c 'rm -rf ${dst}; mkdir -p ${dst}; chmod 777 ${dst}'"
"${adb_cmd[@]}" push "${LIB_SCUDO}" "${dst}/libscudo_md_preload.so" >/dev/null
"${adb_cmd[@]}" push "${BROKER}" "${dst}/memory_delegation_broker" >/dev/null
"${adb_cmd[@]}" push "${WRAPPER}" "${dst}/md_bili_scudo_wrap.sh" >/dev/null
"${adb_cmd[@]}" push "${PHONE_SCRIPT}" "${dst}/recover_bili_scudo_overlay_phone.sh" >/dev/null
"${adb_cmd[@]}" shell "su -c 'chmod 755 ${dst}/recover_bili_scudo_overlay_phone.sh; WORK=${dst} PKG=${PKG} BIND_WRAP_TARGET=${BIND_WRAP_TARGET} sh ${dst}/recover_bili_scudo_overlay_phone.sh'"
