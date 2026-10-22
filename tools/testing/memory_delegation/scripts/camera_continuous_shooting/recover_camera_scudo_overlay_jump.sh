#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Jump-host side driver for the camera Scudo overlay recovery flow.

set -euo pipefail

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
REMOTE_DIR="${REMOTE_DIR:?REMOTE_DIR is required}"
PHONE_UPDATE_DIR="${PHONE_UPDATE_DIR:-/data/local/tmp/md-camera-scudo-update}"

adb_cmd=("${ADB_BIN}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

case "${REMOTE_DIR}" in
  /*) src="${REMOTE_DIR}" ;;
  *) src="${HOME}/${REMOTE_DIR}" ;;
esac
dst="${PHONE_UPDATE_DIR}"

echo "adb: wait-for-device"
"${adb_cmd[@]}" wait-for-device
echo "adb: prepare ${dst}"
"${adb_cmd[@]}" shell "rm -rf '${dst}' 2>/dev/null || su -c 'rm -rf ${dst}'"
"${adb_cmd[@]}" shell "mkdir -p '${dst}' && chmod 777 '${dst}'"

for f in \
  libscudo_md_android.so \
  libscudo_md_preload.so \
  memory_delegation_broker \
  md_camera_scudo_wrap.sh \
  service.sh \
  recover_camera_scudo_overlay_phone.sh
do
  echo "adb: push ${f}"
  "${adb_cmd[@]}" push "${src}/${f}" "${dst}/${f}" >/dev/null
done

"${adb_cmd[@]}" shell "chmod 755 '${dst}/recover_camera_scudo_overlay_phone.sh'"
echo "adb: run copied phone script"
"${adb_cmd[@]}" shell "su -c 'sh ${dst}/recover_camera_scudo_overlay_phone.sh'"
export ADB_SERVER_SOCKET
