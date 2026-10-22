#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
TESTS_DIR="${ROOT_DIR}/tools/testing/memory_delegation/tests"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-}"
ADB=( "${ADB_BIN}" )
if [[ -n "${ADB_SERIAL}" ]]; then
  ADB+=( -s "${ADB_SERIAL}" )
fi

TRACE_FILE="${TRACE_FILE:-}"
if [[ -z "${TRACE_FILE}" ]]; then
  TRACE_FILE="$(ls -t /tmp/md-camera-run/alloc-trace-*/md_camera_alloc_trace.csv 2>/dev/null | head -n 1 || true)"
fi
[[ -f "${TRACE_FILE}" ]] || { echo "missing TRACE_FILE=${TRACE_FILE}" >&2; exit 1; }

MODE="${MODE:-delegated}"
TOUCH="${TOUCH:-none}"
MAX_EVENTS="${MAX_EVENTS:-0}"
REPEAT="${REPEAT:-1}"
TOUCH_DELAY_US="${TOUCH_DELAY_US:-0}"
DELEGATED_RESET_ON_INIT="${DELEGATED_RESET_ON_INIT:-1}"
PROFILE="${PROFILE:-0}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_alloc_trace_replay}"
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-/tmp/md-alloc-trace-replay-$(date +%Y%m%d-%H%M%S)}"
BUILD="${BUILD:-0}"

mkdir -p "${LOCAL_OUT_DIR}"

PROFILE_BUILD_ARGS=()
if [[ "${PROFILE}" = "1" ]]; then
  PROFILE_BUILD_ARGS=(SCUDO_SHARED_ARENA_PROFILE_BUILD=1)
fi

if [[ "${BUILD}" = "1" || ! -x "${TESTS_DIR}/out/md_alloc_trace_replay" ]]; then
  make -C "${TESTS_DIR}" MODE=android "${PROFILE_BUILD_ARGS[@]}" md_alloc_trace_replay memory_delegation_broker md_mem_pressure_app
fi

"${ADB[@]}" wait-for-device
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}'"
"${ADB[@]}" push "${TESTS_DIR}/out/md_alloc_trace_replay" "${REMOTE_DIR}/md_alloc_trace_replay" >/dev/null
"${ADB[@]}" push "${TESTS_DIR}/out/memory_delegation_broker" "${REMOTE_DIR}/memory_delegation_broker" >/dev/null
"${ADB[@]}" push "${TRACE_FILE}" "${REMOTE_DIR}/trace.csv" >/dev/null

PHONE_SCRIPT="${LOCAL_OUT_DIR}/run_alloc_trace_replay_phone.sh"
cat >"${PHONE_SCRIPT}" <<'EOS'
#!/system/bin/sh
set -eu
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_alloc_trace_replay}"
MODE="${MODE:-delegated}"
TOUCH="${TOUCH:-none}"
MAX_EVENTS="${MAX_EVENTS:-0}"
REPEAT="${REPEAT:-1}"
TOUCH_DELAY_US="${TOUCH_DELAY_US:-0}"
DELEGATED_RESET_ON_INIT="${DELEGATED_RESET_ON_INIT:-1}"
PROFILE="${PROFILE:-0}"
PROFILE_ENV=""
if [ "$PROFILE" = "1" ]; then
  PROFILE_ENV="SCUDO_SHARED_ARENA_PROFILE=1"
fi

cd "$REMOTE_DIR"
chmod 755 ./md_alloc_trace_replay ./memory_delegation_broker
rm -f broker.log replay.log

if [ "$MODE" = "delegated" ]; then
  ./memory_delegation_broker --trace --ashmem >broker.log 2>&1 &
  broker_pid="$!"
  sleep 1
  env $PROFILE_ENV \
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \
  SCUDO_SHARED_ARENA_FORCE=1 \
  SCUDO_SHARED_ARENA_RESET_ON_INIT="$DELEGATED_RESET_ON_INIT" \
  SCUDO_SHARED_ARENA_TRACE=0 \
  ./md_alloc_trace_replay --trace trace.csv --touch "$TOUCH" \
    --max-events "$MAX_EVENTS" --repeat "$REPEAT" \
    --touch-delay-us "$TOUCH_DELAY_US" >replay.log 2>&1
  rc="$?"
  kill "$broker_pid" 2>/dev/null || true
  wait "$broker_pid" 2>/dev/null || true
else
  SCUDO_SHARED_ARENA_DISABLE=1 \
  ./md_alloc_trace_replay --trace trace.csv --touch "$TOUCH" \
    --max-events "$MAX_EVENTS" --repeat "$REPEAT" \
    --touch-delay-us "$TOUCH_DELAY_US" >replay.log 2>&1
  rc="$?"
fi
cat replay.log
exit "$rc"
EOS

"${ADB[@]}" push "${PHONE_SCRIPT}" "${REMOTE_DIR}/run_alloc_trace_replay_phone.sh" >/dev/null

set +e
"${ADB[@]}" shell "cd '${REMOTE_DIR}' && MODE='${MODE}' TOUCH='${TOUCH}' MAX_EVENTS='${MAX_EVENTS}' REPEAT='${REPEAT}' TOUCH_DELAY_US='${TOUCH_DELAY_US}' DELEGATED_RESET_ON_INIT='${DELEGATED_RESET_ON_INIT}' PROFILE='${PROFILE}' sh ./run_alloc_trace_replay_phone.sh" | tee "${LOCAL_OUT_DIR}/stdout.log"
rc=${PIPESTATUS[0]}
set -e

"${ADB[@]}" pull "${REMOTE_DIR}/replay.log" "${LOCAL_OUT_DIR}/replay.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull "${REMOTE_DIR}/broker.log" "${LOCAL_OUT_DIR}/broker.log" >/dev/null 2>&1 || true
echo "rc=${rc}" | tee "${LOCAL_OUT_DIR}/result.txt"
echo "out=${LOCAL_OUT_DIR}"
exit "${rc}"
