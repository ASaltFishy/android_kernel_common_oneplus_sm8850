#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build and install a disabled-by-default KernelSU module for testing Scudo as
# Android's bionic malloc_debug replacement.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"
MODULE_DIR="${SCRIPT_DIR}/modules/md-system-scudo"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_UPDATE_DIR="${PHONE_UPDATE_DIR:-/data/local/tmp/md-system-scudo-module-update}"
BUILD="${BUILD:-1}"
export ADB_SERVER_SOCKET

PHONE_SCRIPT="${SCRIPT_DIR}/install_system_scudo_module_phone.sh"

LIB_SCUDO="${TESTS_DIR}/out/libscudo_md_android.so"
BROKER="${TESTS_DIR}/out/memory_delegation_broker"

if [[ "${BUILD}" == "1" ]]; then
  make -B -C "${TESTS_DIR}" MODE=android \
    libscudo_md_android_so memory_delegation_broker
fi

for f in \
  "${MODULE_DIR}/module.prop" \
  "${MODULE_DIR}/post-fs-data.sh" \
  "${MODULE_DIR}/service.sh" \
  "${LIB_SCUDO}" \
  "${BROKER}" \
  "${PHONE_SCRIPT}"; do
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
"${adb_cmd[@]}" push "${MODULE_DIR}/module.prop" "${PHONE_UPDATE_DIR}/module.prop" >/dev/null
"${adb_cmd[@]}" push "${MODULE_DIR}/post-fs-data.sh" "${PHONE_UPDATE_DIR}/post-fs-data.sh" >/dev/null
"${adb_cmd[@]}" push "${MODULE_DIR}/service.sh" "${PHONE_UPDATE_DIR}/service.sh" >/dev/null
"${adb_cmd[@]}" push "${LIB_SCUDO}" "${PHONE_UPDATE_DIR}/libc_malloc_debug.so" >/dev/null
"${adb_cmd[@]}" push "${BROKER}" "${PHONE_UPDATE_DIR}/memory_delegation_broker" >/dev/null
"${adb_cmd[@]}" push "${PHONE_SCRIPT}" "${PHONE_UPDATE_DIR}/install_system_scudo_module_phone.sh" >/dev/null
"${adb_cmd[@]}" shell "su -c 'chmod 755 ${PHONE_UPDATE_DIR}/install_system_scudo_module_phone.sh; TMP=${PHONE_UPDATE_DIR} sh ${PHONE_UPDATE_DIR}/install_system_scudo_module_phone.sh'"
