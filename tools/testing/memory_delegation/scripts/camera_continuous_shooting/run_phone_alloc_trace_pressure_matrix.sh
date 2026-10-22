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

PRESSURE_APPS="${PRESSURE_APPS:-0 1 2}"
PRESSURE_MB_PER_APP="${PRESSURE_MB_PER_APP:-256}"
PRESSURE_CHUNK_KB="${PRESSURE_CHUNK_KB:-1024}"
PRESSURE_STRIDE_KB="${PRESSURE_STRIDE_KB:-4}"
TOUCH="${TOUCH:-none}"
MAX_EVENTS="${MAX_EVENTS:-0}"
REPEAT="${REPEAT:-1}"
DELEGATED_TOUCH_DELAY_US="${DELEGATED_TOUCH_DELAY_US:-0}"
DEFAULT_TOUCH_DELAY_US="${DEFAULT_TOUCH_DELAY_US:-0}"
LIVE_HOLD_MS="${LIVE_HOLD_MS:-2000}"
PSS_SAMPLE_INTERVAL="${PSS_SAMPLE_INTERVAL:-2048}"
DELEGATED_SCUDO_EXTRA_ENV="${DELEGATED_SCUDO_EXTRA_ENV:-}"
DELEGATED_REPLAY_SCUDO_EXTRA_ENV="${DELEGATED_REPLAY_SCUDO_EXTRA_ENV:-}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_alloc_trace_matrix}"
LOCAL_OUT_DIR="${LOCAL_OUT_DIR:-/tmp/md-alloc-trace-pressure-matrix-$(date +%Y%m%d-%H%M%S)}"
BUILD="${BUILD:-0}"

mkdir -p "${LOCAL_OUT_DIR}"

if [[ "${BUILD}" = "1" || ! -x "${TESTS_DIR}/out/md_alloc_trace_replay" || ! -x "${TESTS_DIR}/out/md_mem_pressure_app" ]]; then
  make -C "${TESTS_DIR}" MODE=android md_alloc_trace_replay md_mem_pressure_app memory_delegation_broker
fi

"${ADB[@]}" wait-for-device
"${ADB[@]}" shell "mkdir -p '${REMOTE_DIR}' '${REMOTE_DIR}/out'"
"${ADB[@]}" push "${TESTS_DIR}/out/md_alloc_trace_replay" "${REMOTE_DIR}/md_alloc_trace_replay" >/dev/null
"${ADB[@]}" push "${TESTS_DIR}/out/md_mem_pressure_app" "${REMOTE_DIR}/md_mem_pressure_app" >/dev/null
"${ADB[@]}" push "${TESTS_DIR}/out/memory_delegation_broker" "${REMOTE_DIR}/memory_delegation_broker" >/dev/null
"${ADB[@]}" push "${TRACE_FILE}" "${REMOTE_DIR}/trace.csv" >/dev/null

PHONE_SCRIPT="${LOCAL_OUT_DIR}/run_alloc_trace_matrix_phone.sh"
cat >"${PHONE_SCRIPT}" <<'EOS'
#!/system/bin/sh
set -eu

REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/md_alloc_trace_matrix}"
PRESSURE_APPS="${PRESSURE_APPS:-0 1 2}"
PRESSURE_MB_PER_APP="${PRESSURE_MB_PER_APP:-256}"
PRESSURE_CHUNK_KB="${PRESSURE_CHUNK_KB:-1024}"
PRESSURE_STRIDE_KB="${PRESSURE_STRIDE_KB:-4}"
TOUCH="${TOUCH:-none}"
MAX_EVENTS="${MAX_EVENTS:-0}"
REPEAT="${REPEAT:-1}"
DELEGATED_TOUCH_DELAY_US="${DELEGATED_TOUCH_DELAY_US:-0}"
DEFAULT_TOUCH_DELAY_US="${DEFAULT_TOUCH_DELAY_US:-0}"
LIVE_HOLD_MS="${LIVE_HOLD_MS:-2000}"
PSS_SAMPLE_INTERVAL="${PSS_SAMPLE_INTERVAL:-2048}"
DELEGATED_SCUDO_EXTRA_ENV="${DELEGATED_SCUDO_EXTRA_ENV:-}"
DELEGATED_REPLAY_SCUDO_EXTRA_ENV="${DELEGATED_REPLAY_SCUDO_EXTRA_ENV:-}"

cd "$REMOTE_DIR"
chmod 755 ./md_alloc_trace_replay ./md_mem_pressure_app ./memory_delegation_broker
rm -rf out
mkdir -p out

kill_pressure() {
  for p in $(pidof md_mem_pressure_app 2>/dev/null); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1
  for p in $(pidof md_mem_pressure_app 2>/dev/null); do
    kill -9 "$p" 2>/dev/null || true
  done
}

kill_by_name() {
  name="$1"
  sig="${2:-TERM}"
  pids="$(pidof "$name" 2>/dev/null || true)"
  [ -n "$pids" ] || return 0
  for p in $pids; do
    kill "-$sig" "$p" 2>/dev/null || true
  done
  if command -v su >/dev/null 2>&1; then
    su -c "for p in $pids; do kill -$sig \"\$p\" 2>/dev/null || true; done" \
      2>/dev/null || true
  fi
}

kv() {
  key="$1"
  awk -v k="$key" '{for (i=1;i<=NF;i++){split($i,a,"="); if(a[1]==k){print a[2]; exit}}}'
}

sample_rollup_file() {
  pid="$1"
  file="/proc/$pid/smaps_rollup"
  [ -n "$pid" ] || {
    echo "0 0"
    return 0
  }
  if [ -r "$file" ]; then
    cat "$file" 2>/dev/null
  elif command -v su >/dev/null 2>&1; then
    su -c "cat '$file'" 2>/dev/null || true
  fi | awk '
      /^Pss:/ {pss=$2}
      /^Rss:/ {rss=$2}
      END {printf "%d %d\n", pss, rss}
    '
}

sample_group_rollup() {
  pss=0
  rss=0
  for pid in "$@"; do
    [ -n "$pid" ] || continue
    vals="$(sample_rollup_file "$pid")"
    set -- $vals
    pss=$((pss + ${1:-0}))
    rss=$((rss + ${2:-0}))
  done
  printf "%d %d\n" "$pss" "$rss"
}

pressure_pids() {
  mode="$1"
  apps="$2"
  cat "out/pressure_${mode}_apps${apps}.pids" 2>/dev/null || true
}

sum_pressure_mem() {
  mode="$1"
  apps="$2"
  total_pss=0
  total_rss=0
  i=0
  while [ "$i" -lt "$apps" ]; do
    log="out/pressure_${mode}_apps${apps}_${i}.log"
    vals="$(awk '
      /^MD_PRESSURE_MEM / {
        pss=0; rss=0;
        for (i=1; i<=NF; i++) {
          split($i, a, "=");
          if (a[1] == "pss_kb") pss=a[2];
          else if (a[1] == "rss_kb") rss=a[2];
        }
      }
      END { printf "%d %d\n", pss, rss }
    ' "$log" 2>/dev/null || echo "0 0")"
    set -- $vals
    total_pss=$((total_pss + ${1:-0}))
    total_rss=$((total_rss + ${2:-0}))
    i=$((i + 1))
  done
  printf "%d %d\n" "$total_pss" "$total_rss"
}

wait_live_ready() {
  marker="$1"
  pid="$2"
  tries=0
  while [ "$tries" -lt 80 ]; do
    [ -f "$marker" ] && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.05
    tries=$((tries + 1))
  done
  return 1
}

live_ready_pid() {
  marker="$1"
  fallback="$2"
  pid="$(sed -n 's/^pid=//p' "$marker" 2>/dev/null | head -n 1)"
  if [ -n "$pid" ]; then
    echo "$pid"
  else
    echo "$fallback"
  fi
}

wait_group_rollup() {
  replay_pid="$1"
  shift
  tries=0
  while [ "$tries" -lt 40 ]; do
    read pss_kb rss_kb <<EOF4
$(sample_group_rollup "$replay_pid" "$@")
EOF4
    if [ "${rss_kb:-0}" -gt 0 ]; then
      printf "%d %d\n" "${pss_kb:-0}" "${rss_kb:-0}"
      return 0
    fi
    kill -0 "$replay_pid" 2>/dev/null || break
    sleep 0.05
    tries=$((tries + 1))
  done
  printf "0 0\n"
  return 1
}

start_broker() {
  kill_by_name memory_delegation_broker TERM
  sleep 1
  kill_by_name memory_delegation_broker KILL
  rm -f broker.log
  ./memory_delegation_broker --trace --ashmem >broker.log 2>&1 &
  broker_pid="$!"
  sleep 1
  if ! kill -0 "$broker_pid" 2>/dev/null; then
    cat broker.log >&2 2>/dev/null || true
    return 1
  fi
}

stop_broker() {
  if [ "${broker_pid:-0}" != "0" ]; then
    kill "$broker_pid" 2>/dev/null || true
    sleep 1
    kill -9 "$broker_pid" 2>/dev/null || true
    wait "$broker_pid" 2>/dev/null || true
  fi
  broker_pid=0
}

seed_reset() {
  env $DELEGATED_SCUDO_EXTRA_ENV \
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \
  SCUDO_SHARED_ARENA_FORCE=1 \
  SCUDO_SHARED_ARENA_RESET_ON_INIT=1 \
  SCUDO_SHARED_ARENA_TRACE=0 \
  ./md_alloc_trace_replay --trace trace.csv --max-events 1 --touch none \
    >out/seed_reset.log 2>&1 || true
}

start_pressure() {
  mode="$1"
  apps="$2"
  i=0
  while [ "$i" -lt "$apps" ]; do
    log="out/pressure_${mode}_apps${apps}_${i}.log"
    if [ "$mode" = "delegated" ]; then
      env $DELEGATED_SCUDO_EXTRA_ENV \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT=0 \
      SCUDO_SHARED_ARENA_TRACE=0 \
      ./md_mem_pressure_app --mb "$PRESSURE_MB_PER_APP" \
        --chunk-kb "$PRESSURE_CHUNK_KB" --stride-kb "$PRESSURE_STRIDE_KB" \
        --loop >"$log" 2>&1 &
    else
      SCUDO_SHARED_ARENA_DISABLE=1 \
      ./md_mem_pressure_app --mb "$PRESSURE_MB_PER_APP" \
        --chunk-kb "$PRESSURE_CHUNK_KB" --stride-kb "$PRESSURE_STRIDE_KB" \
        --loop >"$log" 2>&1 &
    fi
    echo "$!" >>"out/pressure_${mode}_apps${apps}.pids"
    i=$((i + 1))
  done
  sleep 3
}

donor_ready_count() {
  mode="$1"
  apps="$2"
  ready=0
  i=0
  while [ "$i" -lt "$apps" ]; do
    log="out/pressure_${mode}_apps${apps}_${i}.log"
    if grep -q "ready" "$log" 2>/dev/null || grep -q "arena_allocs" "$log" 2>/dev/null; then
      ready=$((ready + 1))
    fi
    i=$((i + 1))
  done
  echo "$ready"
}

stop_pressure() {
  kill_pressure
}

run_one() {
  mode="$1"
  apps="$2"
  delay="$DEFAULT_TOUCH_DELAY_US"
  env_prefix="SCUDO_SHARED_ARENA_DISABLE=1"
  if [ "$mode" = "delegated" ]; then
    delay="$DELEGATED_TOUCH_DELAY_US"
    env_prefix="$DELEGATED_SCUDO_EXTRA_ENV $DELEGATED_REPLAY_SCUDO_EXTRA_ENV SCUDO_ALLOC_STATS_FILE=out/scudo_stats_${mode}_apps${apps}.log SCUDO_SHARED_ARENA_EAGER_INIT=1 SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_RESET_ON_INIT=0 SCUDO_SHARED_ARENA_TRACE=0"
  fi

  log="out/replay_${mode}_apps${apps}.log"
  live_ready="out/replay_${mode}_apps${apps}.live_ready"
  rm -f "$live_ready"
  start_ns="$(date +%s%N)"
  set +e
  env $env_prefix ./md_alloc_trace_replay --trace trace.csv --touch "$TOUCH" \
    --max-events "$MAX_EVENTS" --repeat "$REPEAT" \
    --touch-delay-us "$delay" --hold-live-ms "$LIVE_HOLD_MS" \
    --pss-sample-interval "$PSS_SAMPLE_INTERVAL" \
    --live-ready-file "$live_ready" >"$log" 2>&1 &
  replay_pid="$!"
  pressure_pss_kb=0
  pressure_rss_kb=0
  read pressure_pss_kb pressure_rss_kb <<EOF3
$(sum_pressure_mem "$mode" "$apps")
EOF3
  if wait_live_ready "$live_ready" "$replay_pid"; then
    replay_sample_pid="$(live_ready_pid "$live_ready" "$replay_pid")"
    :
  fi
  wait "$replay_pid"
  rc="$?"
  set -e
  end_ns="$(date +%s%N)"
  wall_ms=$(( (end_ns - start_ns) / 1000000 ))
  summary="$(grep 'REPLAY_SUMMARY' "$log" | tail -n 1 || true)"
  events="$(echo "$summary" | kv events)"
  elapsed="$(echo "$summary" | kv elapsed_ms)"
  events_per_s="$(echo "$summary" | kv events_per_s)"
  requested_mb="$(echo "$summary" | kv requested_mb)"
  requested_mb_per_s="$(echo "$summary" | kv requested_mb_per_s)"
  touched_mb="$(echo "$summary" | kv touched_mb)"
  touched_mb_per_s="$(echo "$summary" | kv touched_mb_per_s)"
  maxrss_mb="$(echo "$summary" | kv maxrss_mb)"
  delegated_arena_in_use_mb="$(echo "$summary" | kv delegated_arena_in_use_mb)"
  delegated_arena_free_reusable_mb="$(echo "$summary" | kv delegated_arena_free_reusable_mb)"
  delegated_arena_ready="$(echo "$summary" | kv delegated_arena_ready)"
  arena_alloc_attempts="$(echo "$summary" | kv arena_alloc_attempts)"
  arena_alloc_success="$(echo "$summary" | kv arena_alloc_success)"
  arena_alloc_fallback="$(echo "$summary" | kv arena_alloc_fallback)"
  replay_live_pss_mb="$(echo "$summary" | kv replay_live_pss_mb)"
  replay_live_rss_mb="$(echo "$summary" | kv replay_live_rss_mb)"
  replay_peak_pss_mb="$(echo "$summary" | kv replay_peak_pss_mb)"
  delegated_arena_in_use_at_peak_pss_mb="$(echo "$summary" | kv delegated_arena_in_use_at_peak_pss_mb)"
  alloc_failures="$(echo "$summary" | kv alloc_failures)"
  [ -n "$events" ] || events=0
  [ -n "$elapsed" ] || elapsed=0
  [ -n "$events_per_s" ] || events_per_s=0
  [ -n "$requested_mb" ] || requested_mb=0
  [ -n "$requested_mb_per_s" ] || requested_mb_per_s=0
  [ -n "$touched_mb" ] || touched_mb=0
  [ -n "$touched_mb_per_s" ] || touched_mb_per_s=0
  [ -n "$maxrss_mb" ] || maxrss_mb=0
  [ -n "$delegated_arena_in_use_mb" ] || delegated_arena_in_use_mb=0
  [ -n "$delegated_arena_free_reusable_mb" ] || delegated_arena_free_reusable_mb=0
  [ -n "$delegated_arena_ready" ] || delegated_arena_ready=0
  [ -n "$arena_alloc_attempts" ] || arena_alloc_attempts=0
  [ -n "$arena_alloc_success" ] || arena_alloc_success=0
  [ -n "$arena_alloc_fallback" ] || arena_alloc_fallback=0
  [ -n "$replay_live_pss_mb" ] || replay_live_pss_mb=0
  [ -n "$replay_live_rss_mb" ] || replay_live_rss_mb=0
  [ -n "$replay_peak_pss_mb" ] || replay_peak_pss_mb=0
  [ -n "$delegated_arena_in_use_at_peak_pss_mb" ] || delegated_arena_in_use_at_peak_pss_mb=0
  [ -n "$alloc_failures" ] || alloc_failures=0
  donor_ready="$(donor_ready_count "$mode" "$apps")"
  group_pss_mb="$(awk "BEGIN{printf \"%.3f\", $replay_live_pss_mb + ($pressure_pss_kb / 1024)}")"
  group_rss_mb="$(awk "BEGIN{printf \"%.3f\", $replay_live_rss_mb + ($pressure_rss_kb / 1024)}")"
  group_peak_pss_mb="$(awk "BEGIN{printf \"%.3f\", $replay_peak_pss_mb + ($pressure_pss_kb / 1024)}")"
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%.3f,%.3f,%.3f,%s,%s,%s,%s,%s,%s\n" \
    "$mode" "$apps" "$((apps * PRESSURE_MB_PER_APP))" "$donor_ready" "$rc" \
    "$events" "$elapsed" "$events_per_s" "$requested_mb" "$requested_mb_per_s" \
    "$touched_mb" "$touched_mb_per_s" "$maxrss_mb" \
    "$delegated_arena_in_use_mb" "$delegated_arena_free_reusable_mb" \
    "$delegated_arena_ready" \
    "$arena_alloc_attempts" "$arena_alloc_success" "$arena_alloc_fallback" \
    "$group_pss_mb" \
    "$group_rss_mb" \
    "$group_peak_pss_mb" \
    "$delegated_arena_in_use_at_peak_pss_mb" \
    "$TOUCH" "$wall_ms" "$REPEAT" "$MAX_EVENTS" "$alloc_failures" >>out/matrix_results.csv
  echo "$mode apps=$apps rc=$rc events=$events requested_mb=$requested_mb elapsed_ms=$elapsed events_per_s=$events_per_s alloc_failures=$alloc_failures donor_ready=$donor_ready arena_ready=$delegated_arena_ready arena_in_use_mb=$delegated_arena_in_use_mb arena_free_reusable_mb=$delegated_arena_free_reusable_mb"
}

echo "mode,apps,pressure_mb,donor_ready,rc,events,elapsed_ms,events_per_s,requested_mb,requested_mb_per_s,touched_mb,touched_mb_per_s,maxrss_mb,delegated_arena_in_use_mb,delegated_arena_free_reusable_mb,delegated_arena_ready,arena_alloc_attempts,arena_alloc_success,arena_alloc_fallback,group_pss_mb,group_rss_mb,group_peak_pss_mb,delegated_arena_in_use_at_peak_pss_mb,touch,wall_ms,repeat,max_events,alloc_failures" >out/matrix_results.csv

kill_pressure
for mode in default delegated; do
  for apps in $PRESSURE_APPS; do
    if [ "$mode" = "delegated" ]; then
      start_broker
      seed_reset
    fi
    start_pressure "$mode" "$apps"
    run_one "$mode" "$apps"
    stop_pressure
    if [ "$mode" = "delegated" ]; then
      cp broker.log "out/broker_${mode}_apps${apps}.log" 2>/dev/null || true
      stop_broker
    fi
  done
done
kill_pressure
EOS

"${ADB[@]}" push "${PHONE_SCRIPT}" "${REMOTE_DIR}/run_alloc_trace_matrix_phone.sh" >/dev/null

set +e
"${ADB[@]}" shell "cd '${REMOTE_DIR}' && REMOTE_DIR='${REMOTE_DIR}' PRESSURE_APPS='${PRESSURE_APPS}' PRESSURE_MB_PER_APP='${PRESSURE_MB_PER_APP}' PRESSURE_CHUNK_KB='${PRESSURE_CHUNK_KB}' PRESSURE_STRIDE_KB='${PRESSURE_STRIDE_KB}' TOUCH='${TOUCH}' MAX_EVENTS='${MAX_EVENTS}' REPEAT='${REPEAT}' DELEGATED_TOUCH_DELAY_US='${DELEGATED_TOUCH_DELAY_US}' DEFAULT_TOUCH_DELAY_US='${DEFAULT_TOUCH_DELAY_US}' LIVE_HOLD_MS='${LIVE_HOLD_MS}' PSS_SAMPLE_INTERVAL='${PSS_SAMPLE_INTERVAL}' DELEGATED_SCUDO_EXTRA_ENV='${DELEGATED_SCUDO_EXTRA_ENV}' DELEGATED_REPLAY_SCUDO_EXTRA_ENV='${DELEGATED_REPLAY_SCUDO_EXTRA_ENV}' sh ./run_alloc_trace_matrix_phone.sh" | tee "${LOCAL_OUT_DIR}/stdout.log"
rc=${PIPESTATUS[0]}
set -e

"${ADB[@]}" pull "${REMOTE_DIR}/out" "${LOCAL_OUT_DIR}/remote" >/dev/null 2>&1 || true
if [[ -f "${LOCAL_OUT_DIR}/remote/matrix_results.csv" ]]; then
  cp "${LOCAL_OUT_DIR}/remote/matrix_results.csv" "${LOCAL_OUT_DIR}/matrix_results.csv"
fi

echo "out=${LOCAL_OUT_DIR}"
exit "${rc}"
