#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build and install a disabled-by-default KernelSU module for testing Scudo as
# Android's bionic malloc_debug replacement.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"
MODULE_DIR="${BASE_DIR}/kernelsu_modules/md-system-scudo"

JUMP_HOST="${JUMP_HOST:?JUMP_HOST environment variable must be set (e.g., lrc@192.168.61.230)}"
ADB_BIN="${ADB_BIN:-/opt/homebrew/bin/adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_UPDATE_DIR="${PHONE_UPDATE_DIR:-/data/local/tmp/md-system-scudo-module-update}"
BUILD="${BUILD:-1}"
SSH_OPTS="${SSH_OPTS:-"-o StrictHostKeyChecking=accept-new -o BatchMode=yes"}"

STAMP="$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="${REMOTE_DIR:-tmp/md-system-scudo-module-${STAMP}}"
PHONE_SCRIPT="${SCRIPT_DIR}/phone/install_system_scudo_module_phone.sh"

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

ssh ${SSH_OPTS} "${JUMP_HOST}" "mkdir -p ${REMOTE_DIR}"
scp ${SSH_OPTS} -q "${MODULE_DIR}/module.prop" "${JUMP_HOST}:${REMOTE_DIR}/module.prop"
scp ${SSH_OPTS} -q "${MODULE_DIR}/post-fs-data.sh" "${JUMP_HOST}:${REMOTE_DIR}/post-fs-data.sh"
scp ${SSH_OPTS} -q "${MODULE_DIR}/service.sh" "${JUMP_HOST}:${REMOTE_DIR}/service.sh"
scp ${SSH_OPTS} -q "${LIB_SCUDO}" "${JUMP_HOST}:${REMOTE_DIR}/libc_malloc_debug.so"
scp ${SSH_OPTS} -q "${BROKER}" "${JUMP_HOST}:${REMOTE_DIR}/memory_delegation_broker"
scp ${SSH_OPTS} -q "${PHONE_SCRIPT}" "${JUMP_HOST}:${REMOTE_DIR}/install_system_scudo_module_phone.sh"

ssh ${SSH_OPTS} "${JUMP_HOST}" \
  "ADB_BIN=${ADB_BIN@Q}; ADB_SERIAL=${ADB_SERIAL@Q}; REMOTE_DIR=${REMOTE_DIR@Q}; PHONE_UPDATE_DIR=${PHONE_UPDATE_DIR@Q}; \
   adb_cmd=(\"\${ADB_BIN}\"); if [[ -n \"\${ADB_SERIAL}\" ]]; then adb_cmd+=( -s \"\${ADB_SERIAL}\" ); fi; \
   \"\${adb_cmd[@]}\" wait-for-device; \
   \"\${adb_cmd[@]}\" shell su -c \"rm -rf '\${PHONE_UPDATE_DIR}'; mkdir -p '\${PHONE_UPDATE_DIR}'; chmod 777 '\${PHONE_UPDATE_DIR}'\"; \
   for f in module.prop post-fs-data.sh service.sh libc_malloc_debug.so memory_delegation_broker install_system_scudo_module_phone.sh; do \
     \"\${adb_cmd[@]}\" push \"\${REMOTE_DIR}/\${f}\" \"\${PHONE_UPDATE_DIR}/\${f}\" >/dev/null; \
   done; \
   \"\${adb_cmd[@]}\" shell \"su -c 'chmod 755 \${PHONE_UPDATE_DIR}/install_system_scudo_module_phone.sh; TMP=\${PHONE_UPDATE_DIR} sh \${PHONE_UPDATE_DIR}/install_system_scudo_module_phone.sh'\""
