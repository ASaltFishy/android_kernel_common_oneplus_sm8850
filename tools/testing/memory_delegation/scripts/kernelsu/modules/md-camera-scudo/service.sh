#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/local/tmp/md_camera_scudo_module.log
WRAP_TARGET=/system/bin/bugreportz
SCUDO_TARGET=/system/lib64/libaconfig_test_rust_library.dylib.so

while [ "$(getprop sys.boot_completed)" != "1" ]; do
  sleep 1
done

mkdir -p /data/local/tmp/md
{
  echo "service start $(date)"
  echo "MODDIR=$MODDIR"
  ls -l "$WRAP_TARGET" "$SCUDO_TARGET" 2>&1 || true
  mount | grep -E "$WRAP_TARGET|$SCUDO_TARGET" 2>&1 || true
} >> "$LOG" 2>&1

for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

if [ -x "$MODDIR/system/bin/memory_delegation_broker" ]; then
  "$MODDIR/system/bin/memory_delegation_broker" --trace --ashmem \
    >/data/local/tmp/md/camera_scudo_broker.log 2>&1 &
  echo "broker started pid=$!" >> "$LOG" 2>&1
else
  echo "broker missing from module dir" >> "$LOG" 2>&1
fi
