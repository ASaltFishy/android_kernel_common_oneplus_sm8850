#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build/push the Scudo malloc-debug replacement and run the phone-side live
# overlay script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_UPDATE_DIR="${PHONE_UPDATE_DIR:-/data/local/tmp/md-system-scudo-update}"
BUILD="${BUILD:-1}"
OPTIONS="${OPTIONS:-md_shared,md_trace}"
PROGRAM="${PROGRAM:-}"
export ADB_SERVER_SOCKET

PHONE_SCRIPT="${SCRIPT_DIR}/recover_system_scudo_overlay_phone.sh"

LIB_SCUDO="${TESTS_DIR}/out/libscudo_md_android.so"
BROKER="${TESTS_DIR}/out/memory_delegation_broker"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android \
    libscudo_md_android_so memory_delegation_broker
fi

for f in "${LIB_SCUDO}" "${BROKER}" "${PHONE_SCRIPT}"; do
  [[ -f "${f}" ]] || {
    echo "missing required file: ${f}" >&2
    exit 1
  }
done

adb_cmd=("${ADB_BIN}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

echo "adb: wait-for-device (ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET})"
"${adb_cmd[@]}" wait-for-device
"${adb_cmd[@]}" shell su -c "rm -rf '${PHONE_UPDATE_DIR}'; mkdir -p '${PHONE_UPDATE_DIR}'; chmod 777 '${PHONE_UPDATE_DIR}'"
"${adb_cmd[@]}" push "${LIB_SCUDO}" "${PHONE_UPDATE_DIR}/libscudo_md_android.so" >/dev/null
"${adb_cmd[@]}" push "${BROKER}" "${PHONE_UPDATE_DIR}/memory_delegation_broker" >/dev/null
"${adb_cmd[@]}" push "${PHONE_SCRIPT}" "${PHONE_UPDATE_DIR}/recover_system_scudo_overlay_phone.sh" >/dev/null
"${adb_cmd[@]}" shell "su -c 'chmod 755 ${PHONE_UPDATE_DIR}/recover_system_scudo_overlay_phone.sh; TMP=${PHONE_UPDATE_DIR} OPTIONS=${OPTIONS} PROGRAM=${PROGRAM} sh ${PHONE_UPDATE_DIR}/recover_system_scudo_overlay_phone.sh'"
