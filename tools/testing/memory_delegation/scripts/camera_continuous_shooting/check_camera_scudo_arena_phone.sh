#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Phone-side checker for the OPlus camera Scudo preload experiment.

set -u

CAMERA_PKG="${CAMERA_PKG:-com.oplus.camera}"
WRAP_TARGET="${WRAP_TARGET:-/system/bin/bugreportz}"
SCUDO_TARGET="${SCUDO_TARGET:-/system/lib64/libaconfig_test_rust_library.dylib.so}"
MODULE_DIR="${MODULE_DIR:-/data/adb/modules/md-camera-scudo}"

echo "== module/wrap state =="
printf "wrap.%s=" "$CAMERA_PKG"
getprop "wrap.$CAMERA_PKG"
printf "module_disable="
if [ -e "$MODULE_DIR/disable" ]; then
  echo yes
else
  echo no
fi
echo "-- bind mounts --"
mount | grep "$WRAP_TARGET" || true
mount | grep "$SCUDO_TARGET" || true
echo "-- broker --"
ps -A | grep memory_delegation_broker || true

pid="$(pidof "$CAMERA_PKG" 2>/dev/null | awk '{print $1}')"
if [ -z "$pid" ]; then
  echo "ERROR: camera process not running: $CAMERA_PKG"
  exit 2
fi

echo "== camera process =="
echo "pid=$pid"
cmdline="$(tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)"
echo "cmdline=$cmdline"

echo "== camera env =="
tr '\000' '\n' <"/proc/$pid/environ" 2>/dev/null |
  grep -E '^(LD_PRELOAD|SCUDO_SHARED_ARENA|SCUDO_ALLOC_STATS|WRAP|ANDROID_)' || true

maps="/proc/$pid/maps"
echo "== camera maps summary =="
printf "preload_so_maps="
grep -c "$SCUDO_TARGET" "$maps" 2>/dev/null || true
printf "arena_memfd_maps="
grep -c "memfd:scudo_arena_" "$maps" 2>/dev/null || true
printf "arena_ashmem_maps="
grep -c "/dev/ashmem/scudo_arena_" "$maps" 2>/dev/null || true
printf "arena_any_maps="
grep -c "scudo_arena_" "$maps" 2>/dev/null || true
printf "arena_base_1000000000_maps="
grep -c "1000000000" "$maps" 2>/dev/null || true
printf "scudo_secondary_maps="
grep -c "scudo:secondary" "$maps" 2>/dev/null || true

echo "-- preload so lines --"
grep "$SCUDO_TARGET" "$maps" 2>/dev/null || true
echo "-- arena memfd lines --"
grep "memfd:scudo_arena_" "$maps" 2>/dev/null | head -n 32 || true
echo "-- arena ashmem lines --"
grep "/dev/ashmem/scudo_arena_" "$maps" 2>/dev/null | head -n 32 || true
echo "-- 0x1000000000-ish lines --"
grep "1000000000" "$maps" 2>/dev/null | head -n 32 || true
echo "-- scudo secondary sample --"
grep "scudo:secondary" "$maps" 2>/dev/null | head -n 16 || true

echo "== logs =="
echo "-- broker log tail --"
tail -n 80 /data/local/tmp/md/camera_scudo_broker.log 2>/dev/null || true
echo "-- wrapper log tail --"
tail -n 40 /data/local/tmp/md_camera_scudo_wrap.log 2>/dev/null || true
echo "-- module log tail --"
tail -n 60 /data/local/tmp/md_camera_scudo_module.log 2>/dev/null || true
echo "-- scudo trace file tail --"
tail -n 160 /data/local/tmp/md_camera_scudo_trace.log 2>/dev/null || true
tail -n 160 "/data/data/${CAMERA_PKG}/cache/md_camera_scudo_trace.log" 2>/dev/null || true
echo "-- scudo alloc stats tail --"
tail -n 40 /data/local/tmp/md_camera_scudo_alloc_stats.log 2>/dev/null || true
tail -n 40 "/data/data/${CAMERA_PKG}/cache/md_camera_scudo_alloc_stats.log" 2>/dev/null || true
echo "-- logcat hints --"
logcat -d -v time 2>/dev/null |
  grep -Ei '\[scudo\]|shared_arena|scudo_arena|memory_delegation|broker fd|arena init|pool init|allocate hit|retrieve|store' |
  tail -n 120 || true
