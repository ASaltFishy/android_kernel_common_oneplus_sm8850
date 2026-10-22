#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0

LOG=/data/local/tmp/md_bili_scudo_wrap.log

{
  echo "wrap invoked $(date)"
  echo "uid=$(id)"
  echo "argc=$#"
  echo "args=$*"
} >> "$LOG" 2>&1

exec /system/bin/logwrapper /system/bin/env \
  LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
  SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
  SCUDO_SHARED_ARENA_EAGER_INIT=1 \
  SCUDO_SHARED_ARENA_FORCE=1 \
  SCUDO_SHARED_ARENA_DISABLE_FREELIST=1 \
  SCUDO_SHARED_ARENA_DISABLE_FILL_ALLOC=1 \
  SCUDO_SHARED_ARENA_YIELD_AFTER_ALLOC_LOG=1 \
  SCUDO_SHARED_ARENA_TRACE=1 \
  SCUDO_SHARED_ARENA_TRACE_FILE=/data/local/tmp/md_bili_scudo_trace.log \
  SCUDO_ALLOC_STATS=1 \
  SCUDO_ALLOC_STATS_INTERVAL=4096 \
  "$@"
