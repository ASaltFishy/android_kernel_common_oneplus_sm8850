#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Verify whether the OPlus camera process is running through the KernelSU
# Scudo preload experiment and whether SharedArena mappings are visible.

set -euo pipefail

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
CAMERA_PKG="${CAMERA_PKG:-com.oplus.camera}"
REMOTE_SCRIPT="${REMOTE_SCRIPT:-/data/local/tmp/md_check_camera_scudo_arena.sh}"
PHONE_SCRIPT="${PHONE_SCRIPT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/check_camera_scudo_arena_phone.sh}"

export ADB_SERVER_SOCKET

[[ -f "${PHONE_SCRIPT}" ]] || {
  echo "missing phone checker: ${PHONE_SCRIPT}" >&2
  exit 1
}

adb_cmd=("${ADB_BIN}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

echo "adb: wait-for-device (ADB_SERVER_SOCKET=${ADB_SERVER_SOCKET})"
"${adb_cmd[@]}" wait-for-device
"${adb_cmd[@]}" push -q "${PHONE_SCRIPT}" "${REMOTE_SCRIPT}" >/dev/null
"${adb_cmd[@]}" shell chmod 755 "${REMOTE_SCRIPT}"
"${adb_cmd[@]}" shell su -c "CAMERA_PKG='${CAMERA_PKG}' '${REMOTE_SCRIPT}'"
