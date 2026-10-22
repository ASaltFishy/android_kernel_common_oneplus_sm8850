#!/system/bin/sh
LOG=/data/local/tmp/md_camera_scudo_wrap.log
APP_CACHE=/data/data/com.oplus.camera/cache
TRACE_FILE="$APP_CACHE/md_camera_scudo_trace.log"
ALLOC_STATS_FILE="$APP_CACHE/md_camera_scudo_alloc_stats.log"
RECORD_FILE="$APP_CACHE/md_camera_alloc_events.csv"
ALLOC_TRACE_FILE="$APP_CACHE/md_camera_alloc_trace.csv"

MODE="$(getprop debug.md.camera_scudo.mode 2>/dev/null)"
[ -n "$MODE" ] || MODE=delegated
STATS_INTERVAL="$(getprop debug.md.camera_scudo.stats_interval 2>/dev/null)"
[ -n "$STATS_INTERVAL" ] || STATS_INTERVAL=0
ALLOC_STATS="$(getprop debug.md.camera_scudo.alloc_stats 2>/dev/null)"
[ -n "$ALLOC_STATS" ] || ALLOC_STATS=0
RESET_ON_INIT="$(getprop debug.md.camera_scudo.reset_on_init 2>/dev/null)"
[ -n "$RESET_ON_INIT" ] || RESET_ON_INIT=0
ALLOC_TRACE="$(getprop debug.md.camera_scudo.alloc_trace 2>/dev/null)"
[ -n "$ALLOC_TRACE" ] || ALLOC_TRACE=0
ALLOC_TRACE_MAX_EVENTS="$(getprop debug.md.camera_scudo.alloc_trace_max_events 2>/dev/null)"
[ -n "$ALLOC_TRACE_MAX_EVENTS" ] || ALLOC_TRACE_MAX_EVENTS=2000000

{
  echo "wrap invoked $(date)"
  echo "uid=$(id)"
  echo "mode=$MODE"
  echo "stats_interval=$STATS_INTERVAL"
  echo "alloc_stats=$ALLOC_STATS"
  echo "reset_on_init=$RESET_ON_INIT"
  echo "alloc_trace=$ALLOC_TRACE"
  echo "alloc_trace_max_events=$ALLOC_TRACE_MAX_EVENTS"
  echo "argc=$#"
  echo "args=$*"
} >> "$LOG" 2>&1

case "$MODE" in
  default|scudo-default|baseline)
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_DISABLE=1 \
      SCUDO_SHARED_ARENA_TRACE=0 \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
  delegated|sharedarena|mem-delegated|delegated-fast|sharedarena-fast|mem-delegated-fast)
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT="$RESET_ON_INIT" \
      SCUDO_SHARED_ARENA_TRACE=0 \
      SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
  delegated-nofill|sharedarena-nofill|mem-delegated-nofill)
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT="$RESET_ON_INIT" \
      SCUDO_SHARED_ARENA_DISABLE_FILL_ALLOC=1 \
      SCUDO_SHARED_ARENA_TRACE=0 \
      SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
  delegated-nofreelist|sharedarena-nofreelist|mem-delegated-nofreelist)
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT="$RESET_ON_INIT" \
      SCUDO_SHARED_ARENA_DISABLE_FREELIST=1 \
      SCUDO_SHARED_ARENA_TRACE=0 \
      SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
  delegated-debug|sharedarena-debug|mem-delegated-debug)
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT="$RESET_ON_INIT" \
      SCUDO_SHARED_ARENA_DISABLE_FREELIST=1 \
      SCUDO_SHARED_ARENA_DISABLE_FILL_ALLOC=1 \
      SCUDO_SHARED_ARENA_YIELD_AFTER_ALLOC_LOG=1 \
      SCUDO_SHARED_ARENA_TRACE=1 \
      SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
  *)
    echo "unknown mode=$MODE, fallback delegated" >> "$LOG" 2>&1
    exec /system/bin/logwrapper /system/bin/env \
      LD_LIBRARY_PATH=/data/adb/modules/md-camera-scudo/system/lib64:/system/lib64 \
      LD_PRELOAD=/system/lib64/libaconfig_test_rust_library.dylib.so \
      SCUDO_OPTIONS=delete_size_mismatch=false:dealloc_type_mismatch=false \
      SCUDO_SHARED_ARENA_EAGER_INIT=1 \
      SCUDO_SHARED_ARENA_FORCE=1 \
      SCUDO_SHARED_ARENA_RESET_ON_INIT="$RESET_ON_INIT" \
      SCUDO_SHARED_ARENA_TRACE=0 \
      SCUDO_SHARED_ARENA_TRACE_FILE="$TRACE_FILE" \
      SCUDO_SHARED_ARENA_RECORD_FILE="$RECORD_FILE" \
      SCUDO_ALLOC_TRACE="$ALLOC_TRACE" \
      SCUDO_ALLOC_TRACE_FILE="$ALLOC_TRACE_FILE" \
      SCUDO_ALLOC_TRACE_HEADER=1 \
      SCUDO_ALLOC_TRACE_MAX_EVENTS="$ALLOC_TRACE_MAX_EVENTS" \
      SCUDO_ALLOC_STATS="$ALLOC_STATS" \
      SCUDO_ALLOC_STATS_INTERVAL="$STATS_INTERVAL" \
      SCUDO_ALLOC_STATS_FILE="$ALLOC_STATS_FILE" \
      "$@"
    ;;
esac
