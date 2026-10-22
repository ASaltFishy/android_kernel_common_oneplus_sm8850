#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Phone-side live overlay for the multi-app switch latency experiment.
# This keeps the global system allocator untouched and only enables per-package
# wrap.<pkg> LD_PRELOAD for the listed applications.

set -eu

WORK="${WORK:-/data/local/tmp/md-bili-scudo}"
WRAP_TARGET="${WRAP_TARGET:-/system/bin/bugreportz}"
SO_TARGET="${SO_TARGET:-/system/lib64/libaconfig_test_rust_library.dylib.so}"
WRAPPER="${WORK}/md_app_switch_scudo_wrap.sh"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_app_switch_scudo_trace.log}"
WRAP_LOG="${WRAP_LOG:-/data/local/tmp/md_app_switch_scudo_wrap.log}"
PKGS="${PKGS:-tv.danmaku.bili com.tencent.qqmusic com.taobao.taobao com.tencent.qqlive com.alibaba.wireless com.baidu.searchbox com.dragon.read com.autonavi.minimap com.sina.weibo com.xingin.xhs com.ss.android.ugc.aweme}"

require_file() {
  if [ ! -f "$1" ]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

require_file "$WORK/libscudo_md_preload.so"
require_file "$WORK/memory_delegation_broker"

cat >"$WRAPPER" <<EOF
#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0

LOG="$WRAP_LOG"

{
  echo "wrap invoked \$(date)"
  echo "uid=\$(id)"
  echo "argc=\$#"
  echo "args=\$*"
} >> "\$LOG" 2>&1

exec /system/bin/logwrapper /system/bin/env \\
  LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \\
  SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \\
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \\
  SCUDO_SHARED_ARENA_FORCE=1 \\
  SCUDO_SHARED_ARENA_DISABLE_FREELIST=1 \\
  SCUDO_SHARED_ARENA_DISABLE_FILL_ALLOC=1 \\
  SCUDO_SHARED_ARENA_YIELD_AFTER_ALLOC_LOG=1 \\
  SCUDO_SHARED_ARENA_TRACE=1 \\
  SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \\
  SCUDO_ALLOC_STATS=1 \\
  SCUDO_ALLOC_STATS_INTERVAL=4096 \\
  "\$@"
EOF

for pkg in $PKGS; do
  am force-stop "$pkg" 2>/dev/null || true
done

for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

chmod 755 "$WORK/libscudo_md_preload.so" \
          "$WORK/memory_delegation_broker" \
          "$WRAPPER"
chcon u:object_r:system_file:s0 "$WORK/libscudo_md_preload.so" \
  "$WORK/memory_delegation_broker" "$WRAPPER" \
  2>/dev/null || true

umount "$WRAP_TARGET" 2>/dev/null || true
umount "$SO_TARGET" 2>/dev/null || true
mount --bind "$WRAPPER" "$WRAP_TARGET"
mount --bind "$WORK/libscudo_md_preload.so" "$SO_TARGET"

for pkg in $PKGS; do
  setprop "wrap.$pkg" "$WRAP_TARGET"
done

rm -f "$TRACE_FILE" "$WRAP_LOG" "$WORK/app_switch_broker.log"
: > "$TRACE_FILE"
chmod 666 "$TRACE_FILE" 2>/dev/null || true

cd "$WORK"
./memory_delegation_broker --trace --ashmem >app_switch_broker.log 2>&1 &
broker_pid="$!"

echo "app switch scudo overlay restored"
echo "broker_pid=$broker_pid"
echo "-- wrap props --"
for pkg in $PKGS; do
  printf "wrap.%s=" "$pkg"
  getprop "wrap.$pkg"
done
echo "-- bind mounts --"
mount | grep "$WRAP_TARGET" || true
mount | grep "$SO_TARGET" || true
echo "-- broker log --"
tail -n 40 "$WORK/app_switch_broker.log" 2>/dev/null || true
