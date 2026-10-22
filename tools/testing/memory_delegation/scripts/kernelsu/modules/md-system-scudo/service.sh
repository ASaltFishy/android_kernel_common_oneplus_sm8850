#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Start the SharedArena broker after Android boot.  This script only runs when
# the KernelSU module is explicitly enabled.

MODDIR=${0%/*}
LOG=/data/local/tmp/md_system_scudo_module.log
BROKER="$MODDIR/system/bin/memory_delegation_broker"
BROKER_LOG=/data/local/tmp/md/system_scudo_broker.log

while [ "$(getprop sys.boot_completed)" != "1" ]; do
  sleep 1
done

mkdir -p /data/local/tmp/md
{
  echo "service start $(date)"
  echo "MODDIR=$MODDIR"
  echo "BROKER=$BROKER"
} >> "$LOG" 2>&1

for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -TERM "$p" || true
done
sleep 1
for p in $(pidof memory_delegation_broker 2>/dev/null); do
  kill -KILL "$p" || true
done

if [ -x "$BROKER" ]; then
  "$BROKER" --trace --ashmem >"$BROKER_LOG" 2>&1 &
  echo "broker started pid=$!" >> "$LOG" 2>&1
else
  echo "broker missing or not executable" >> "$LOG" 2>&1
fi
