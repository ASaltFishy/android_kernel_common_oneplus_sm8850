#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Build/push/run the camera SharedArena donor helper on a phone. This is a
# quick smoke for the donor process used before real camera burst experiments.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_DIR="${PHONE_DIR:-/data/local/tmp/md_camera_donor}"
BUILD="${BUILD:-1}"
USE_SU="${USE_SU:-1}"

DONOR_CPU="${DONOR_CPU:-0}"
DONOR_CHUNKS="${DONOR_CHUNKS:-64}"
DONOR_CHUNK_KB="${DONOR_CHUNK_KB:-1024}"
DONOR_HOLD_SECS="${DONOR_HOLD_SECS:-1}"
DONOR_TOUCH_ROUNDS="${DONOR_TOUCH_ROUNDS:-1}"

BIN_DONOR="${TESTS_DIR}/out/md_camera_arena_donor"
BIN_BROKER="${TESTS_DIR}/out/memory_delegation_broker"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android md_camera_arena_donor memory_delegation_broker
fi

[[ -f "${BIN_DONOR}" ]] || {
  echo "missing ${BIN_DONOR}" >&2
  exit 1
}
[[ -f "${BIN_BROKER}" ]] || {
  echo "missing ${BIN_BROKER}" >&2
  exit 1
}

export ADB_SERVER_SOCKET

adb_cmd=("${ADB_BIN}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

run_shell() {
  if [[ "${USE_SU}" == "1" ]]; then
    "${adb_cmd[@]}" shell "su -c $(printf "%q" "$1")" </dev/null
  else
    "${adb_cmd[@]}" shell "sh -c $(printf "%q" "$1")" </dev/null
  fi
}

"${adb_cmd[@]}" wait-for-device </dev/null
"${adb_cmd[@]}" shell "mkdir -p '${PHONE_DIR}' && chmod 755 '${PHONE_DIR}'" </dev/null
"${adb_cmd[@]}" push "${BIN_DONOR}" "${PHONE_DIR}/md_camera_arena_donor" >/dev/null
"${adb_cmd[@]}" push "${BIN_BROKER}" "${PHONE_DIR}/memory_delegation_broker" >/dev/null
"${adb_cmd[@]}" shell "chmod 0755 '${PHONE_DIR}/md_camera_arena_donor' '${PHONE_DIR}/memory_delegation_broker'" </dev/null

"${adb_cmd[@]}" shell "for p in \$(pidof md_camera_arena_donor 2>/dev/null) \$(pidof memory_delegation_broker 2>/dev/null); do kill -TERM \"\$p\" || true; done; sleep 1; for p in \$(pidof md_camera_arena_donor 2>/dev/null) \$(pidof memory_delegation_broker 2>/dev/null); do kill -KILL \"\$p\" || true; done" </dev/null || true

"${adb_cmd[@]}" shell "cd '${PHONE_DIR}' && rm -f broker.log && (./memory_delegation_broker --trace --ashmem >broker.log 2>&1 < /dev/null &) ; echo broker_started" </dev/null
for _ in $(seq 1 10); do
  if "${adb_cmd[@]}" shell "grep -q 'ready num_cores=' '${PHONE_DIR}/broker.log'" </dev/null 2>/dev/null; then
    break
  fi
  sleep 1
done
"${adb_cmd[@]}" shell "cat '${PHONE_DIR}/broker.log' 2>/dev/null | tail -n 20" </dev/null || true

run_one() {
  local mode="$1"
  local extra_env="$2"
  echo "== donor mode=${mode} =="
  run_shell "cd '${PHONE_DIR}' && ${extra_env} MD_CAMERA_DONOR_CPU='${DONOR_CPU}' MD_CAMERA_DONOR_CHUNKS='${DONOR_CHUNKS}' MD_CAMERA_DONOR_CHUNK_KB='${DONOR_CHUNK_KB}' MD_CAMERA_DONOR_HOLD_SECS='${DONOR_HOLD_SECS}' MD_CAMERA_DONOR_TOUCH_ROUNDS='${DONOR_TOUCH_ROUNDS}' ./md_camera_arena_donor" |
    sed "s/^/[${mode}] /"
}

run_one delegated "SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=0"
run_one default "SCUDO_SHARED_ARENA_DISABLE=1 SCUDO_SHARED_ARENA_TRACE=0"

"${adb_cmd[@]}" shell "for p in \$(pidof md_camera_arena_donor 2>/dev/null) \$(pidof memory_delegation_broker 2>/dev/null); do kill -TERM \"\$p\" || true; done" </dev/null || true
