#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Bind the module Scudo malloc-debug shim into Android runtime APEX paths.
# This script only runs when the KernelSU module is explicitly enabled.

MODDIR=${0%/*}
LOG=/data/local/tmp/md_system_scudo_module.log

SCUDO_SO="$MODDIR/system/lib64/libc_malloc_debug.so"
TARGET64=/apex/com.android.runtime/lib64/libc_malloc_debug.so
TARGET64_ALT=/apex/com.android.runtime@1/lib64/libc_malloc_debug.so

mkdir -p /data/local/tmp/md
{
  echo "post-fs-data start $(date)"
  echo "MODDIR=$MODDIR"
  echo "SCUDO_SO=$SCUDO_SO"
} >> "$LOG" 2>&1

if [ ! -f "$SCUDO_SO" ]; then
  echo "missing $SCUDO_SO" >> "$LOG" 2>&1
  exit 0
fi

chcon u:object_r:system_file:s0 "$SCUDO_SO" 2>/dev/null || true

if [ -f "$TARGET64" ]; then
  umount "$TARGET64" 2>/dev/null || true
  mount --bind "$SCUDO_SO" "$TARGET64" >> "$LOG" 2>&1 || true
fi

if [ -f "$TARGET64_ALT" ]; then
  umount "$TARGET64_ALT" 2>/dev/null || true
  mount --bind "$SCUDO_SO" "$TARGET64_ALT" >> "$LOG" 2>&1 || true
fi

# Keep program empty only after an explicit enable: all new 64-bit processes
# become candidates for malloc-debug dispatch.  Options use stock bionic
# spelling by default; service.sh/broker env decide SharedArena behavior.
setprop libc.debug.malloc.options "${MD_SYSTEM_SCUDO_OPTIONS:-backtrace}"
setprop libc.debug.malloc.program "${MD_SYSTEM_SCUDO_PROGRAM:-}"

{
  echo "options=$(getprop libc.debug.malloc.options)"
  echo "program=$(getprop libc.debug.malloc.program)"
  mount | grep -E "$TARGET64|$TARGET64_ALT" || true
} >> "$LOG" 2>&1
