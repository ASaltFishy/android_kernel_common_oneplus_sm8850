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

REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_shared_arena_cross_core_free}"
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-/tmp/md-shared-arena-cross-core-free-$(date +%Y%m%d-%H%M%S)}"
BUILD="${BUILD:-0}"
SIZES="${SIZES:-262144}"
ITERATIONS="${ITERATIONS:-200}"
WARMUP="${WARMUP:-20}"
BATCH_SIZE="${BATCH_SIZE:-32}"
CPU_PAIRS="${CPU_PAIRS:-0:0 0:1 0:6}"
TOUCH_PAGES="${TOUCH_PAGES:-0}"
PROFILE="${PROFILE:-1}"

mkdir -p "${LOCAL_OUT_DIR}"

PROFILE_BUILD_ARGS=()
if [[ "${PROFILE}" = "1" ]]; then
  PROFILE_BUILD_ARGS=(SCUDO_SHARED_ARENA_PROFILE_BUILD=1)
fi

if [[ "${BUILD}" = "1" || ! -x "${TESTS_DIR}/out/scudo_shared_arena_latency_bench" || ! -x "${TESTS_DIR}/out/memory_delegation_broker" ]]; then
  make -C "${TESTS_DIR}" MODE=android "${PROFILE_BUILD_ARGS[@]}" scudo_shared_arena_latency_bench memory_delegation_broker
fi

"${ADB[@]}" wait-for-device
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}'"
"${ADB[@]}" push "${TESTS_DIR}/out/scudo_shared_arena_latency_bench" "${REMOTE_DIR}/scudo_shared_arena_latency_bench" >/dev/null
"${ADB[@]}" push "${TESTS_DIR}/out/memory_delegation_broker" "${REMOTE_DIR}/memory_delegation_broker" >/dev/null

PHONE_SCRIPT="${LOCAL_OUT_DIR}/run_shared_arena_cross_core_free_phone.sh"
cat >"${PHONE_SCRIPT}" <<'EOS'
#!/system/bin/sh
set -eu

REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_shared_arena_cross_core_free}"
SIZES="${SIZES:-262144}"
ITERATIONS="${ITERATIONS:-200}"
WARMUP="${WARMUP:-20}"
BATCH_SIZE="${BATCH_SIZE:-32}"
CPU_PAIRS="${CPU_PAIRS:-0:0 0:1 0:6}"
TOUCH_PAGES="${TOUCH_PAGES:-0}"
PROFILE="${PROFILE:-1}"
PROFILE_ENV=""
if [ "$PROFILE" = "1" ]; then
  PROFILE_ENV="SCUDO_SHARED_ARENA_PROFILE=1"
fi

cd "$REMOTE_DIR"
chmod 755 ./scudo_shared_arena_latency_bench ./memory_delegation_broker
rm -f broker.log cross-core-free.log

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

for pair in $CPU_PAIRS; do
  alloc_cpu="${pair%:*}"
  free_cpu="${pair#*:}"
  echo "===== cross-core-free alloc_cpu=${alloc_cpu} free_cpu=${free_cpu} sizes=${SIZES} iterations=${ITERATIONS} warmup=${WARMUP} batch=${BATCH_SIZE} touch=${TOUCH_PAGES} =====" | tee -a cross-core-free.log
  env $PROFILE_ENV \
  MALLOC_USE_APP_DEFAULTS=1 \
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \
  SCUDO_SHARED_ARENA_FORCE=1 \
  SCUDO_SHARED_ARENA_RESET_ON_INIT=1 \
  SCUDO_SHARED_ARENA_TRACE=0 \
  ./scudo_shared_arena_latency_bench \
    --cross-core-free 1 \
    --alloc-cpu "$alloc_cpu" \
    --free-cpu "$free_cpu" \
    --batch-size "$BATCH_SIZE" \
    --iterations "$ITERATIONS" \
    --warmup "$WARMUP" \
    --touch-pages "$TOUCH_PAGES" \
    --touch-pages-warmup "$TOUCH_PAGES" \
    --sizes "$SIZES" 2>&1 | tee -a cross-core-free.log
done
EOS

"${ADB[@]}" push "${PHONE_SCRIPT}" "${REMOTE_DIR}/run_shared_arena_cross_core_free_phone.sh" >/dev/null

set +e
"${ADB[@]}" shell "cd '${REMOTE_DIR}' && REMOTE_DIR='${REMOTE_DIR}' SIZES='${SIZES}' ITERATIONS='${ITERATIONS}' WARMUP='${WARMUP}' BATCH_SIZE='${BATCH_SIZE}' CPU_PAIRS='${CPU_PAIRS}' TOUCH_PAGES='${TOUCH_PAGES}' PROFILE='${PROFILE}' sh ./run_shared_arena_cross_core_free_phone.sh" | tee "${LOCAL_OUT_DIR}/stdout.log"
rc=${PIPESTATUS[0]}
set -e

"${ADB[@]}" pull "${REMOTE_DIR}/cross-core-free.log" "${LOCAL_OUT_DIR}/cross-core-free.log" >/dev/null 2>&1 || true
"${ADB[@]}" pull "${REMOTE_DIR}/broker.log" "${LOCAL_OUT_DIR}/broker.log" >/dev/null 2>&1 || true

echo "rc=${rc}" | tee "${LOCAL_OUT_DIR}/result.txt"
echo "out=${LOCAL_OUT_DIR}"
exit "${rc}"
