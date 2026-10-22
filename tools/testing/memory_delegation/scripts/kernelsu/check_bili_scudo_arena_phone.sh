#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Phone-side checker for the Bilibili Scudo preload experiment.

set -u

PKG="${PKG:-tv.danmaku.bili}"
WORK="${WORK:-/data/local/tmp/md-bili-scudo}"
WRAP_TARGET="${WRAP_TARGET:-/system/bin/bugreportz}"
SO_TARGET="${SO_TARGET:-/system/lib64/libaconfig_test_rust_library.dylib.so}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_bili_scudo_trace.log}"
WRAP_LOG="${WRAP_LOG:-/data/local/tmp/md_bili_scudo_wrap.log}"

echo "== module/wrap state =="
printf "wrap.%s=" "$PKG"
getprop "wrap.$PKG"
echo "-- bind mounts --"
mount | grep "$WRAP_TARGET" || true
mount | grep "$SO_TARGET" || true
echo "-- broker --"
ps -A | grep memory_delegation_broker || true

pid="$(pidof "$PKG" 2>/dev/null | awk '{print $1}')"
if [ -z "$pid" ]; then
  echo "ERROR: process not running: $PKG"
  echo "-- related bili processes --"
  ps -A | grep tv.danmaku.bili || true
  exit 2
fi

echo "== bili process =="
echo "pid=$pid"
cmdline="$(tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)"
echo "cmdline=$cmdline"

echo "== bili env =="
tr '\000' '\n' <"/proc/$pid/environ" 2>/dev/null |
  grep -E '^(LD_PRELOAD|SCUDO_SHARED_ARENA|SCUDO_ALLOC_STATS|SCUDO_OPTIONS|WRAP|ANDROID_)' || true

maps="/proc/$pid/maps"
echo "== bili maps summary =="
printf "preload_so_maps="
grep -c "$SO_TARGET" "$maps" 2>/dev/null || true
printf "arena_memfd_maps="
grep -c "memfd:scudo_arena_" "$maps" 2>/dev/null || true
printf "arena_ashmem_maps="
grep -c "/dev/ashmem/scudo_arena_" "$maps" 2>/dev/null || true
printf "arena_any_maps="
grep -c "scudo_arena_" "$maps" 2>/dev/null || true
printf "scudo_secondary_maps="
grep -c "scudo:secondary" "$maps" 2>/dev/null || true

echo "-- preload so lines --"
grep "$SO_TARGET" "$maps" 2>/dev/null || true
echo "-- arena memfd lines --"
grep "memfd:scudo_arena_" "$maps" 2>/dev/null | head -n 32 || true
echo "-- arena ashmem lines --"
grep "/dev/ashmem/scudo_arena_" "$maps" 2>/dev/null | head -n 32 || true
echo "-- scudo secondary sample --"
grep "scudo:secondary" "$maps" 2>/dev/null | head -n 16 || true

echo "== logs =="
echo "-- broker log tail --"
tail -n 100 "$WORK/broker.log" 2>/dev/null || true
echo "-- wrapper log tail --"
tail -n 60 "$WRAP_LOG" 2>/dev/null || true
echo "-- scudo trace file tail --"
tail -n 180 "$TRACE_FILE" 2>/dev/null || true
echo "-- logcat hints --"
logcat -d -v time 2>/dev/null |
  grep -Ei '\[scudo\]|shared_arena|scudo_arena|memory_delegation|broker fd|arena init|pool init|allocate hit|retrieve|store|tv.danmaku.bili' |
  tail -n 160 || true
