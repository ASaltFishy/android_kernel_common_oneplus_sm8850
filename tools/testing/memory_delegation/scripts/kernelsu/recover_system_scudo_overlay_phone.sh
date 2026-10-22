#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Live systemless overlay for testing the memory-delegation Scudo through
# Android bionic's libc_malloc_debug.so dispatch path.

set -eu

TMP="${TMP:-/data/local/tmp/md-system-scudo-update}"
WORK="${WORK:-/data/local/tmp/md-system-scudo}"
TARGET64="${TARGET64:-/apex/com.android.runtime/lib64/libc_malloc_debug.so}"
TARGET64_ALT="${TARGET64_ALT:-/apex/com.android.runtime@1/lib64/libc_malloc_debug.so}"
TARGET32="${TARGET32:-/apex/com.android.runtime/lib/libc_malloc_debug.so}"
BROKER_LOG="${BROKER_LOG:-broker.log}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_system_scudo_trace.log}"
OPTIONS="${OPTIONS:-md_shared,md_trace}"
PROGRAM="${PROGRAM:-}"

require_file() {
  if [ ! -f "$1" ]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

require_file "$TMP/libscudo_md_android.so"
require_file "$TMP/memory_delegation_broker"

umount "$TARGET64" 2>/dev/null || umount -l "$TARGET64" 2>/dev/null || true
if [ -f "$TARGET64_ALT" ]; then
  umount "$TARGET64_ALT" 2>/dev/null || umount -l "$TARGET64_ALT" 2>/dev/null || true
fi

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$TMP/libscudo_md_android.so" "$WORK/libc_malloc_debug.so"
cp "$TMP/memory_delegation_broker" "$WORK/memory_delegation_broker"
chmod 755 "$WORK/libc_malloc_debug.so" "$WORK/memory_delegation_broker"
chcon u:object_r:system_file:s0 "$WORK/libc_malloc_debug.so" \
  "$WORK/memory_delegation_broker" 2>/dev/null || true

for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

cd "$WORK"
rm -f "$BROKER_LOG"
./memory_delegation_broker --trace --ashmem >"$BROKER_LOG" 2>&1 &
broker_pid="$!"

rm -f "$TRACE_FILE"
: >"$TRACE_FILE"
chmod 666 "$TRACE_FILE" 2>/dev/null || true

mount --bind "$WORK/libc_malloc_debug.so" "$TARGET64"
if [ -f "$TARGET64_ALT" ]; then
  mount --bind "$WORK/libc_malloc_debug.so" "$TARGET64_ALT"
fi

# 32-bit processes still use the stock debug malloc unless a 32-bit build is
# provided.  Leave TARGET32 untouched for this aarch64 smoke.
setprop libc.debug.malloc.options "$OPTIONS"
setprop libc.debug.malloc.program "$PROGRAM"

echo "system scudo overlay restored"
echo "target64=$TARGET64"
echo "target64_alt=$TARGET64_ALT"
echo "options=$(getprop libc.debug.malloc.options)"
echo "program=$(getprop libc.debug.malloc.program)"
echo "broker_pid=$broker_pid"
mount | grep "$TARGET64" || true
mount | grep "$TARGET64_ALT" || true
tail -n 40 "$WORK/$BROKER_LOG" 2>/dev/null || true
