#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

PKG="${PKG:-tv.danmaku.bili}"
WORK="${WORK:-/data/local/tmp/md-bili-scudo}"
WRAP_TARGET="${WRAP_TARGET:-/system/bin/bugreportz}"
WRAP_PROP_VALUE="${WRAP_PROP_VALUE:-$WORK/md_bili_scudo_wrap.sh}"
BIND_WRAP_TARGET="${BIND_WRAP_TARGET:-0}"
SO_TARGET="${SO_TARGET:-/system/lib64/libaconfig_test_rust_library.dylib.so}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_bili_scudo_trace.log}"
WRAP_LOG="${WRAP_LOG:-/data/local/tmp/md_bili_scudo_wrap.log}"

require_file() {
  if [ ! -f "$1" ]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

require_file "$WORK/libscudo_md_preload.so"
require_file "$WORK/memory_delegation_broker"
require_file "$WORK/md_bili_scudo_wrap.sh"

am force-stop "$PKG" 2>/dev/null || true
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

chmod 755 "$WORK/libscudo_md_preload.so" \
          "$WORK/memory_delegation_broker" \
          "$WORK/md_bili_scudo_wrap.sh"
chcon u:object_r:system_file:s0 "$WORK/libscudo_md_preload.so" \
  "$WORK/memory_delegation_broker" "$WORK/md_bili_scudo_wrap.sh" \
  2>/dev/null || true

umount "$SO_TARGET" 2>/dev/null || umount -l "$SO_TARGET" 2>/dev/null || true
mount --bind "$WORK/libscudo_md_preload.so" "$SO_TARGET"
if [ "$BIND_WRAP_TARGET" = "1" ]; then
  umount "$WRAP_TARGET" 2>/dev/null || umount -l "$WRAP_TARGET" 2>/dev/null || true
  mount --bind "$WORK/md_bili_scudo_wrap.sh" "$WRAP_TARGET"
  WRAP_PROP_VALUE="$WRAP_TARGET"
fi
setprop "wrap.$PKG" "$WRAP_PROP_VALUE"

rm -f "$TRACE_FILE" "$WRAP_LOG" "$WORK/broker.log"
: > "$TRACE_FILE"
chmod 666 "$TRACE_FILE" 2>/dev/null || true

cd "$WORK"
./memory_delegation_broker --trace --ashmem >broker.log 2>&1 &
broker_pid="$!"

echo "bili overlay restored"
echo "wrap.$PKG=$(getprop "wrap.$PKG")"
echo "broker_pid=$broker_pid"
echo "wrapper=$WRAP_PROP_VALUE"
mount | grep "$WRAP_TARGET" || true
mount | grep "$SO_TARGET" || true
tail -n 40 "$WORK/broker.log" 2>/dev/null || true
