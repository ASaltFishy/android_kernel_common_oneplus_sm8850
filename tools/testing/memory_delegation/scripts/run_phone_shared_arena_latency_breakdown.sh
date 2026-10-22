#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
TESTS_DIR="${ROOT_DIR}/tools/testing/memory_delegation/tests"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-}"
ADB=( "${ADB_BIN}" )
if [[ -n "${ADB_SERIAL}" ]]; then
  ADB+=( -s "${ADB_SERIAL}" )
fi

REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_shared_arena_latency_breakdown}"
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-/tmp/md-shared-arena-latency-breakdown-$(date +%Y%m%d-%H%M%S)}"
BUILD="${BUILD:-0}"
SIZES="${SIZES:-65536,262144,1048576,4194304}"
NO_TOUCH_ITERATIONS="${NO_TOUCH_ITERATIONS:-2000}"
NO_TOUCH_WARMUP="${NO_TOUCH_WARMUP:-200}"
TOUCH_ITERATIONS="${TOUCH_ITERATIONS:-1000}"
TOUCH_WARMUP="${TOUCH_WARMUP:-100}"
THREADS="${THREADS:-1}"
ALLOCATOR="${ALLOCATOR:-secondary}"
BENCH_MODE="${BENCH_MODE:-thread}"
PATH_MODE="${PATH_MODE:-both}"
HOLD_US="${HOLD_US:-0}"
DELEGATED_RESET_ON_INIT="${DELEGATED_RESET_ON_INIT:-1}"

mkdir -p "${LOCAL_OUT_DIR}"

if [[ "${BUILD}" = "1" || ! -x "${TESTS_DIR}/out/scudo_shared_arena_latency_bench" || ! -x "${TESTS_DIR}/out/memory_delegation_broker" ]]; then
  make -C "${TESTS_DIR}" MODE=android scudo_shared_arena_latency_bench memory_delegation_broker
fi

"${ADB[@]}" wait-for-device
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}'"
"${ADB[@]}" push "${TESTS_DIR}/out/scudo_shared_arena_latency_bench" "${REMOTE_DIR}/scudo_shared_arena_latency_bench" >/dev/null
"${ADB[@]}" push "${TESTS_DIR}/out/memory_delegation_broker" "${REMOTE_DIR}/memory_delegation_broker" >/dev/null

PHONE_SCRIPT="${LOCAL_OUT_DIR}/run_shared_arena_latency_breakdown_phone.sh"
cat >"${PHONE_SCRIPT}" <<'EOS'
#!/system/bin/sh
set -eu

REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_shared_arena_latency_breakdown}"
SIZES="${SIZES:-65536,262144,1048576,4194304}"
NO_TOUCH_ITERATIONS="${NO_TOUCH_ITERATIONS:-2000}"
NO_TOUCH_WARMUP="${NO_TOUCH_WARMUP:-200}"
TOUCH_ITERATIONS="${TOUCH_ITERATIONS:-1000}"
TOUCH_WARMUP="${TOUCH_WARMUP:-100}"
THREADS="${THREADS:-1}"
ALLOCATOR="${ALLOCATOR:-secondary}"
BENCH_MODE="${BENCH_MODE:-thread}"
PATH_MODE="${PATH_MODE:-both}"
HOLD_US="${HOLD_US:-0}"
DELEGATED_RESET_ON_INIT="${DELEGATED_RESET_ON_INIT:-1}"

cd "$REMOTE_DIR"
chmod 755 ./scudo_shared_arena_latency_bench ./memory_delegation_broker
rm -f broker.log no-touch.log touch-pages.log

broker_pid=""
cleanup() {
  if [ -n "$broker_pid" ]; then
    kill "$broker_pid" 2>/dev/null || true
    wait "$broker_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

./memory_delegation_broker --trace --ashmem >broker.log 2>&1 &
broker_pid="$!"
sleep 1

run_case() {
  label="$1"
  touch_pages="$2"
  iterations="$3"
  warmup="$4"
  log_file="$5"

  echo "===== ${label} touch_pages=${touch_pages} iterations=${iterations} warmup=${warmup} sizes=${SIZES} ====="
  MALLOC_USE_APP_DEFAULTS=1 \
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \
  SCUDO_SHARED_ARENA_RESET_ON_INIT="$DELEGATED_RESET_ON_INIT" \
  SCUDO_SHARED_ARENA_TRACE=0 \
  ./scudo_shared_arena_latency_bench \
    --path "$PATH_MODE" \
    --allocator "$ALLOCATOR" \
    --mode "$BENCH_MODE" \
    --threads "$THREADS" \
    --iterations "$iterations" \
    --warmup "$warmup" \
    --hold-us "$HOLD_US" \
    --touch-pages "$touch_pages" \
    --touch-pages-warmup "$touch_pages" \
    --sizes "$SIZES" >"$log_file" 2>&1
  cat "$log_file"
}

run_case no-touch 0 "$NO_TOUCH_ITERATIONS" "$NO_TOUCH_WARMUP" no-touch.log
run_case touch-pages 1 "$TOUCH_ITERATIONS" "$TOUCH_WARMUP" touch-pages.log
EOS

"${ADB[@]}" push "${PHONE_SCRIPT}" "${REMOTE_DIR}/run_shared_arena_latency_breakdown_phone.sh" >/dev/null

set +e
"${ADB[@]}" shell \
  "cd '${REMOTE_DIR}' && REMOTE_DIR='${REMOTE_DIR}' SIZES='${SIZES}' NO_TOUCH_ITERATIONS='${NO_TOUCH_ITERATIONS}' NO_TOUCH_WARMUP='${NO_TOUCH_WARMUP}' TOUCH_ITERATIONS='${TOUCH_ITERATIONS}' TOUCH_WARMUP='${TOUCH_WARMUP}' THREADS='${THREADS}' ALLOCATOR='${ALLOCATOR}' BENCH_MODE='${BENCH_MODE}' PATH_MODE='${PATH_MODE}' HOLD_US='${HOLD_US}' DELEGATED_RESET_ON_INIT='${DELEGATED_RESET_ON_INIT}' sh ./run_shared_arena_latency_breakdown_phone.sh" | tee "${LOCAL_OUT_DIR}/stdout.log"
rc=${PIPESTATUS[0]}
set -e

"${ADB[@]}" pull "${REMOTE_DIR}/no-touch.log" "${LOCAL_OUT_DIR}/no-touch.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull "${REMOTE_DIR}/touch-pages.log" "${LOCAL_OUT_DIR}/touch-pages.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull "${REMOTE_DIR}/broker.log" "${LOCAL_OUT_DIR}/broker.log" >/dev/null 2>&1 || true

echo "rc=${rc}" | tee "${LOCAL_OUT_DIR}/result.txt"
echo "out=${LOCAL_OUT_DIR}"
exit "${rc}"
