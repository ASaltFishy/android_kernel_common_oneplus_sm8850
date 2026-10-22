#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -u

WORK="${WORK:-/data/local/tmp/md-system-scudo}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_system_scudo_trace.log}"

mkdir -p "$WORK"
rm -f "$WORK/smoke.pid"

sh -c 'echo $$ > /data/local/tmp/md-system-scudo/smoke.pid; sleep 30' &
sleep 1

pid="$(cat "$WORK/smoke.pid" 2>/dev/null || true)"
echo "smoke_pid=$pid"
echo "options=$(getprop libc.debug.malloc.options)"
echo "program=$(getprop libc.debug.malloc.program)"

if [ -z "$pid" ] || [ ! -r "/proc/$pid/maps" ]; then
  echo "ERROR: smoke process not inspectable"
  exit 2
fi

echo "== maps =="
grep -E 'libc_malloc_debug|scudo_arena_|libscudo|scudo:secondary' \
  "/proc/$pid/maps" || true

echo "== environ =="
tr '\000' '\n' <"/proc/$pid/environ" 2>/dev/null |
  grep -E '^(SCUDO_SHARED_ARENA|LD_PRELOAD)' || true

echo "== broker =="
tail -n 80 "$WORK/broker.log" 2>/dev/null || true

echo "== trace =="
tail -n 120 "$TRACE_FILE" 2>/dev/null || true
