#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -u

PID_FILE="${PID_FILE:-/data/local/tmp/md-system-scudo-shell.pid}"
WORK="${WORK:-/data/local/tmp/md-system-scudo}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_system_scudo_trace.log}"

pid="$(cat "$PID_FILE" 2>/dev/null || true)"
echo "pid=$pid"
echo "options=$(getprop libc.debug.malloc.options)"
echo "program=$(getprop libc.debug.malloc.program)"

if [ -z "$pid" ] || [ ! -r "/proc/$pid/maps" ]; then
  echo "ERROR: process not inspectable"
  exit 2
fi

echo "== cmdline =="
tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true
echo

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
