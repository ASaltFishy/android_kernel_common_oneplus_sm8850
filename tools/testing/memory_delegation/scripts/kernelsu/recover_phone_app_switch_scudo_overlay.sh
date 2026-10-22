#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build/push the app-switch Scudo preload artifacts and run the phone-side live
# overlay recovery script for the multi-application LMKD/stability experiment.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_WORK_DIR="${PHONE_WORK_DIR:-/data/local/tmp/md-bili-scudo}"
BUILD="${BUILD:-1}"
export ADB_SERVER_SOCKET

PHONE_SCRIPT="${SCRIPT_DIR}/recover_app_switch_scudo_overlay_phone.sh"

LIB_SCUDO="${TESTS_DIR}/out/libscudo_md_preload.so"
BROKER="${TESTS_DIR}/out/memory_delegation_broker"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android \
    libscudo_md_preload_so memory_delegation_broker
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

dst="${PHONE_WORK_DIR}"
echo "adb: wait-for-device (ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET})"
"${adb_cmd[@]}" wait-for-device
"${adb_cmd[@]}" shell "rm -rf '${dst}' 2>/dev/null || su -c 'rm -rf ${dst}'"
"${adb_cmd[@]}" shell "mkdir -p '${dst}' && chmod 777 '${dst}'"
"${adb_cmd[@]}" push "${LIB_SCUDO}" "${dst}/libscudo_md_preload.so" >/dev/null
"${adb_cmd[@]}" push "${BROKER}" "${dst}/memory_delegation_broker" >/dev/null
"${adb_cmd[@]}" push "${PHONE_SCRIPT}" "${dst}/recover_app_switch_scudo_overlay_phone.sh" >/dev/null
"${adb_cmd[@]}" shell "chmod 755 '${dst}/recover_app_switch_scudo_overlay_phone.sh'"
"${adb_cmd[@]}" shell "su -c 'WORK=${dst} sh ${dst}/recover_app_switch_scudo_overlay_phone.sh'"
