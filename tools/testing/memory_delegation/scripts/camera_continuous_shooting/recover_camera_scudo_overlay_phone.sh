#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Restore the live KernelSU overlay used by the OPlus camera Scudo preload
# experiment. This script is intended to be copied to the phone and run via su.

set -eu

MOD="${MOD:-/data/adb/modules/md-camera-scudo}"
TMP="${TMP:-/data/local/tmp/md-camera-scudo-update}"
WRAP_TARGET="${WRAP_TARGET:-/system/bin/bugreportz}"
SCUDO_TARGET="${SCUDO_TARGET:-/system/lib64/libaconfig_test_rust_library.dylib.so}"
MALLOC_DEBUG_TARGET64="${MALLOC_DEBUG_TARGET64:-/apex/com.android.runtime/lib64/libc_malloc_debug.so}"
MALLOC_DEBUG_TARGET64_ALT="${MALLOC_DEBUG_TARGET64_ALT:-/apex/com.android.runtime@1/lib64/libc_malloc_debug.so}"
ENABLE_MALLOC_DEBUG_OVERLAY="${ENABLE_MALLOC_DEBUG_OVERLAY:-0}"
CAMERA_PKG="${CAMERA_PKG:-com.oplus.camera}"
BROKER_LOG_DIR="${BROKER_LOG_DIR:-/data/local/tmp/md}"
BROKER_LOG="${BROKER_LOG:-camera_scudo_broker.log}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_camera_scudo_trace.log}"
WRAP_LOG="${WRAP_LOG:-/data/local/tmp/md_camera_scudo_wrap.log}"
ALLOC_STATS_FILE="${ALLOC_STATS_FILE:-/data/local/tmp/md_camera_scudo_alloc_stats.log}"
APP_CACHE_DIR="${APP_CACHE_DIR:-/data/data/${CAMERA_PKG}/cache}"
APP_TRACE_FILE="${APP_TRACE_FILE:-${APP_CACHE_DIR}/md_camera_scudo_trace.log}"
APP_ALLOC_STATS_FILE="${APP_ALLOC_STATS_FILE:-${APP_CACHE_DIR}/md_camera_scudo_alloc_stats.log}"

require_file() {
  if [ ! -f "$1" ]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

require_file "$TMP/libscudo_md_android.so"
require_file "$TMP/libscudo_md_preload.so"
require_file "$TMP/memory_delegation_broker"
require_file "$TMP/md_camera_scudo_wrap.sh"
require_file "$TMP/service.sh"

am force-stop "$CAMERA_PKG" 2>/dev/null || true
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

mkdir -p "$MOD/system/lib64" "$MOD/system/bin" "$BROKER_LOG_DIR"
cp "$TMP/libscudo_md_android.so" "$MOD/system/lib64/libscudo_md_android.so"
cp "$TMP/libscudo_md_preload.so" "$MOD/system/lib64/libscudo_md_preload.so"
cp "$TMP/memory_delegation_broker" "$MOD/system/bin/memory_delegation_broker"
cp "$TMP/md_camera_scudo_wrap.sh" "$MOD/system/bin/md_camera_scudo_wrap.sh"
cp "$TMP/service.sh" "$MOD/service.sh"

chmod 755 "$MOD/system/bin/memory_delegation_broker" \
          "$MOD/system/bin/md_camera_scudo_wrap.sh" \
          "$MOD/service.sh" \
          "$MOD/system/lib64/libscudo_md_android.so" \
          "$MOD/system/lib64/libscudo_md_preload.so"
chcon u:object_r:system_file:s0 \
  "$MOD/system/lib64/libscudo_md_android.so" \
  "$MOD/system/lib64/libscudo_md_preload.so" \
  "$MOD/system/bin/memory_delegation_broker" \
  "$MOD/system/bin/md_camera_scudo_wrap.sh" \
  "$MOD/service.sh" 2>/dev/null || true

while mount | grep -F -q " on $SCUDO_TARGET "; do
  umount "$SCUDO_TARGET" 2>/dev/null || break
done
while mount | grep -F -q " on $MALLOC_DEBUG_TARGET64 "; do
  umount "$MALLOC_DEBUG_TARGET64" 2>/dev/null || break
done
if [ -f "$MALLOC_DEBUG_TARGET64_ALT" ]; then
  while mount | grep -F -q " on $MALLOC_DEBUG_TARGET64_ALT "; do
    umount "$MALLOC_DEBUG_TARGET64_ALT" 2>/dev/null || break
  done
fi
while mount | grep -F -q " on $WRAP_TARGET "; do
  umount "$WRAP_TARGET" 2>/dev/null || break
done
mount --bind "$MOD/system/lib64/libscudo_md_preload.so" "$SCUDO_TARGET"
if [ "$ENABLE_MALLOC_DEBUG_OVERLAY" = "1" ]; then
  mount --bind "$MOD/system/lib64/libscudo_md_android.so" "$MALLOC_DEBUG_TARGET64"
  if [ -f "$MALLOC_DEBUG_TARGET64_ALT" ]; then
    mount --bind "$MOD/system/lib64/libscudo_md_android.so" "$MALLOC_DEBUG_TARGET64_ALT"
  fi
  setprop libc.debug.malloc.options md_shared
  setprop libc.debug.malloc.program "$CAMERA_PKG"
else
  setprop libc.debug.malloc.options ""
  setprop libc.debug.malloc.program ""
fi
mount --bind "$MOD/system/bin/md_camera_scudo_wrap.sh" "$WRAP_TARGET"
setprop "wrap.$CAMERA_PKG" "$WRAP_TARGET"

cd "$BROKER_LOG_DIR"
rm -f "$BROKER_LOG"
"$MOD/system/bin/memory_delegation_broker" --trace --ashmem \
  >"$BROKER_LOG" 2>&1 &
broker_pid="$!"

rm -f "$TRACE_FILE" "$WRAP_LOG" "$ALLOC_STATS_FILE"
: >"$TRACE_FILE"
: >"$ALLOC_STATS_FILE"
chmod 666 "$TRACE_FILE" 2>/dev/null || true
chmod 666 "$ALLOC_STATS_FILE" 2>/dev/null || true

if mkdir -p "$APP_CACHE_DIR" 2>/dev/null; then
  rm -f "$APP_TRACE_FILE" "$APP_ALLOC_STATS_FILE"
  : >"$APP_TRACE_FILE" 2>/dev/null || true
  : >"$APP_ALLOC_STATS_FILE" 2>/dev/null || true
  chown "$(stat -c %u "/data/data/${CAMERA_PKG}" 2>/dev/null || echo 10172)":"$(stat -c %g "$APP_CACHE_DIR" 2>/dev/null || echo 10172)" \
    "$APP_TRACE_FILE" "$APP_ALLOC_STATS_FILE" 2>/dev/null || true
  chmod 660 "$APP_TRACE_FILE" "$APP_ALLOC_STATS_FILE" 2>/dev/null || true
fi

echo "overlay restored"
echo "wrap.$CAMERA_PKG=$(getprop "wrap.$CAMERA_PKG")"
echo "malloc_debug_options=$(getprop libc.debug.malloc.options)"
echo "malloc_debug_program=$(getprop libc.debug.malloc.program)"
echo "broker_pid=$broker_pid"
mount | grep "$WRAP_TARGET" || true
mount | grep "$SCUDO_TARGET" || true
if [ "$ENABLE_MALLOC_DEBUG_OVERLAY" = "1" ]; then
  mount | grep "$MALLOC_DEBUG_TARGET64" || true
  mount | grep "$MALLOC_DEBUG_TARGET64_ALT" || true
fi
