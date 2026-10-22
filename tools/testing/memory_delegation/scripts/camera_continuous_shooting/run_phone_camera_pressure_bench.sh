#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Camera continuous-shooting experiment over reverse-forwarded adb.
#
# This runner can use Scudo donor helpers to create allocator-cache state before
# the real camera app runs. The preferred mode measures fixed-N-photo completion
# and post-input drain time instead of "photos in a short fixed window", which is
# too sensitive to camera app throttling and media-store writeback jitter.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TESTS_DIR="${BASE_DIR}/tests"

ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:15037}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-3B15AL00K5D00000}"
PHONE_DIR="${PHONE_DIR:-/data/local/tmp/md_camera}"
WORKDIR="${WORKDIR:-/tmp/md-camera-run}"
BUILD="${BUILD:-1}"
USE_SU="${USE_SU:-1}"

VARIANT_LABEL="${VARIANT_LABEL:-sharedarena-kernel}"
PRESSURE_APPS="${PRESSURE_APPS:-0 2 4 6}"
PRESSURE_MB_PER_APP="${PRESSURE_MB_PER_APP:-512}"
PRESSURE_SECONDS="${PRESSURE_SECONDS:-240}"
PRESSURE_TOUCH_MS="${PRESSURE_TOUCH_MS:-1000}"
BACKGROUND_MODE="${BACKGROUND_MODE:-donor}" # donor, pressure, both, none
DONOR_MODE="${DONOR_MODE:-auto}"            # auto, delegated, default
DONOR_CHUNKS="${DONOR_CHUNKS:-0}"           # 0 derives from PRESSURE_MB_PER_APP
DONOR_CHUNK_KB="${DONOR_CHUNK_KB:-1024}"
DONOR_TOUCH_ROUNDS="${DONOR_TOUCH_ROUNDS:-1}"
DONOR_READY_TIMEOUT_SECS="${DONOR_READY_TIMEOUT_SECS:-60}"
DONOR_CPU_MODE="${DONOR_CPU_MODE:-spread}"  # spread or fixed
DONOR_FIXED_CPU="${DONOR_FIXED_CPU:-0}"
ROUNDS="${ROUNDS:-3}"
SETTLE_SECS="${SETTLE_SECS:-30}"
SHOT_SECONDS="${SHOT_SECONDS:-10}"
TARGET_PHOTOS="${TARGET_PHOTOS:-30}"        # 0 keeps legacy fixed-window mode
PHOTO_POLL_EVERY_ATTEMPTS="${PHOTO_POLL_EVERY_ATTEMPTS:-1}"
SHOOT_TIMEOUT_SECS="${SHOOT_TIMEOUT_SECS:-90}"
SHOOT_MODE="${SHOOT_MODE:-tap}"
TAP_INTERVAL_MS="${TAP_INTERVAL_MS:-120}"
POST_WAIT_SECS="${POST_WAIT_SECS:-5}"
DRAIN_STABLE_SECS="${DRAIN_STABLE_SECS:-2}"
DRAIN_TIMEOUT_SECS="${DRAIN_TIMEOUT_SECS:-30}"
CAMERA_COMPONENT="${CAMERA_COMPONENT:-com.oplus.camera/.Camera}"
SHUTTER_X="${SHUTTER_X:-350}"
SHUTTER_Y="${SHUTTER_Y:-636}"
CLEAR_LOGCAT="${CLEAR_LOGCAT:-0}"
PREPARE_DEVICE="${PREPARE_DEVICE:-1}"
GROUP_CLEANUP="${GROUP_CLEANUP:-1}"
ALLOW_PARTIAL_CSV="${ALLOW_PARTIAL_CSV:-0}"
CLEAN_PROCESSES_BETWEEN_ROUNDS="${CLEAN_PROCESSES_BETWEEN_ROUNDS:-1}"
ROUND_CLEANUP_SETTLE_SECS="${ROUND_CLEANUP_SETTLE_SECS:-5}"
RESTART_BROKER_BETWEEN_GROUPS="${RESTART_BROKER_BETWEEN_GROUPS:-0}"
RESTART_BROKER_BETWEEN_ROUNDS="${RESTART_BROKER_BETWEEN_ROUNDS:-0}"
RESTART_BROKER_ON_START="${RESTART_BROKER_ON_START:-1}"
ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS="${ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS:-1}"
CLEAN_PHOTOS_BETWEEN_GROUPS="${CLEAN_PHOTOS_BETWEEN_GROUPS:-1}"
CLEAN_PHOTOS_BETWEEN_ROUNDS="${CLEAN_PHOTOS_BETWEEN_ROUNDS:-1}"
CLEAN_PHOTOS_ON_EXIT="${CLEAN_PHOTOS_ON_EXIT:-1}"
FINAL_PHOTO_CLEANUP_WAIT_SECS="${FINAL_PHOTO_CLEANUP_WAIT_SECS:-5}"
DUMP_STATE_BETWEEN_STEPS="${DUMP_STATE_BETWEEN_STEPS:-1}"
DISMISS_USB_DIALOG="${DISMISS_USB_DIALOG:-1}"
DISMISS_USB_DIALOG_TAPS="${DISMISS_USB_DIALOG_TAPS:-636,2360 636,2180 950,2360 950,2180}"
RUN_BASELINE_DELEGATED_SUITE="${RUN_BASELINE_DELEGATED_SUITE:-0}"
SUITE_PRESSURE_APPS="${SUITE_PRESSURE_APPS:-0 1 2}"
SUITE_PRESSURE_MB_PER_APP="${SUITE_PRESSURE_MB_PER_APP:-256}"
SUITE_LABEL_PREFIX="${SUITE_LABEL_PREFIX:-camera-baseline-delegated}"

mkdir -p "${WORKDIR}"

export ADB_SERVER_SOCKET
STAMP="$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="${WORKDIR}/md-camera-${STAMP}"
OUT_PREFIX="${WORKDIR}/camera-pressure-${VARIANT_LABEL}-${STAMP}"
OUT_LOG="${OUT_PREFIX}.log"
OUT_CSV="${OUT_PREFIX}.csv"
OUT_PNG="${OUT_PREFIX}.png"

BIN_PRESSURE="${TESTS_DIR}/out/md_mem_pressure_app"
BIN_DONOR="${TESTS_DIR}/out/md_camera_arena_donor"

if [[ "${BUILD}" == "1" ]]; then
  make -C "${TESTS_DIR}" MODE=android md_mem_pressure_app md_camera_arena_donor
fi
[[ -f "${BIN_PRESSURE}" ]] || {
  echo "missing ${BIN_PRESSURE}; run BUILD=1 $0" >&2
  exit 1
}
[[ -f "${BIN_DONOR}" ]] || {
  echo "missing ${BIN_DONOR}; run BUILD=1 $0" >&2
  exit 1
}

if [[ "${RUN_BASELINE_DELEGATED_SUITE}" == "1" ]]; then
  adb_cmd=("${ADB_BIN}")
  if [[ -n "${ADB_SERIAL}" ]]; then
    adb_cmd+=( -s "${ADB_SERIAL}" )
  fi

  cleanup_phone_photos() {
    [[ "${GROUP_CLEANUP}" == "1" && "${CLEAN_PHOTOS_ON_EXIT}" == "1" ]] || return 0
    local cleanup_script
    cleanup_script="$(mktemp)"
    cat >"${cleanup_script}" <<'EOS'
#!/system/bin/sh
set -eu

PHONE_DIR="${PHONE_DIR:-/data/local/tmp/md_camera}"
PHOTO_DIR="${PHOTO_DIR:-/sdcard/DCIM/Camera}"
PHOTO_MARKER="${PHOTO_MARKER:-${PHONE_DIR}/.photo_cleanup_marker}"
PHOTO_SNAPSHOT="${PHOTO_SNAPSHOT:-${PHONE_DIR}/.photo_cleanup_snapshot}"
PHOTO_DELETE_LIST="${PHOTO_DELETE_LIST:-${PHONE_DIR}/.photo_cleanup_delete_list}"
FINAL_PHOTO_CLEANUP_WAIT_SECS="${FINAL_PHOTO_CLEANUP_WAIT_SECS:-5}"

list_photo_files() {
  find "$PHOTO_DIR" -maxdepth 1 -type f \
    \( -name '*.jpg' -o -name '*.jpeg' -o -name '*.heic' \
       -o -name '*.JPG' -o -name '*.JPEG' -o -name '*.HEIC' \) \
    2>/dev/null
}

sleep "$FINAL_PHOTO_CLEANUP_WAIT_SECS" 2>/dev/null || true
[ -f "$PHOTO_MARKER" ] || : >"$PHOTO_MARKER"
: >"$PHOTO_DELETE_LIST" 2>/dev/null || true
list_photo_files |
while IFS= read -r photo; do
  base="${photo##*/}"
  if [ -f "$PHOTO_SNAPSHOT" ] && grep -Fxq "$base" "$PHOTO_SNAPSHOT" 2>/dev/null; then
    if [ "$photo" -nt "$PHOTO_MARKER" ]; then
      echo "$photo"
    fi
  else
    echo "$photo"
  fi
done | sort -u >"$PHOTO_DELETE_LIST" 2>/dev/null || true

deleted=0
while IFS= read -r photo; do
  [ -n "$photo" ] || continue
  rm -f "$photo" 2>/dev/null || true
  deleted=$((deleted + 1))
done <"$PHOTO_DELETE_LIST" 2>/dev/null || true
rm -f "$PHOTO_DELETE_LIST" 2>/dev/null || true
echo "suite: deleted_photos_since_marker=$deleted"
EOS
    "${adb_cmd[@]}" shell "mkdir -p '${PHONE_DIR}' && chmod 755 '${PHONE_DIR}'" </dev/null || true
    "${adb_cmd[@]}" push "${cleanup_script}" "${PHONE_DIR}/cleanup_camera_photos.sh" >/dev/null || true
    rm -f "${cleanup_script}"
    if [[ "${USE_SU}" == "1" ]]; then
      "${adb_cmd[@]}" shell "su -c $(printf "%q" "PHONE_DIR='${PHONE_DIR}' FINAL_PHOTO_CLEANUP_WAIT_SECS='${FINAL_PHOTO_CLEANUP_WAIT_SECS}' sh '${PHONE_DIR}/cleanup_camera_photos.sh'")" </dev/null || true
    else
      "${adb_cmd[@]}" shell "PHONE_DIR='${PHONE_DIR}' FINAL_PHOTO_CLEANUP_WAIT_SECS='${FINAL_PHOTO_CLEANUP_WAIT_SECS}' sh '${PHONE_DIR}/cleanup_camera_photos.sh'" </dev/null || true
    fi
  }

  trap cleanup_phone_photos EXIT

  for mode in default delegated; do
    echo "suite: configure camera scudo mode=${mode}"
    "${adb_cmd[@]}" wait-for-device </dev/null
    "${adb_cmd[@]}" shell \
      "su -c 'setprop debug.md.camera_scudo.mode ${mode}; setprop debug.md.camera_scudo.alloc_stats 0; setprop debug.md.camera_scudo.stats_interval 0'" \
      </dev/null

    ADB_BIN="${ADB_BIN}" ADB_SERIAL="${ADB_SERIAL}" BUILD=0 \
      "${SCRIPT_DIR}/recover_phone_camera_scudo_overlay.sh"

    echo "suite: run mode=${mode} donor apps=${SUITE_PRESSURE_APPS}"
    env \
      RUN_BASELINE_DELEGATED_SUITE=0 \
      BUILD=0 \
      ADB_BIN="${ADB_BIN}" \
      ADB_SERIAL="${ADB_SERIAL}" \
      PHONE_DIR="${PHONE_DIR}" \
      WORKDIR="${WORKDIR}" \
      USE_SU="${USE_SU}" \
      VARIANT_LABEL="${SUITE_LABEL_PREFIX}-${mode}-donor${SUITE_PRESSURE_MB_PER_APP}" \
      BACKGROUND_MODE=donor \
      DONOR_MODE="${mode}" \
      PRESSURE_APPS="${SUITE_PRESSURE_APPS}" \
      PRESSURE_MB_PER_APP="${SUITE_PRESSURE_MB_PER_APP}" \
      PRESSURE_SECONDS="${PRESSURE_SECONDS}" \
      PRESSURE_TOUCH_MS="${PRESSURE_TOUCH_MS}" \
      DONOR_CHUNKS="${DONOR_CHUNKS}" \
      DONOR_CHUNK_KB="${DONOR_CHUNK_KB}" \
      DONOR_TOUCH_ROUNDS="${DONOR_TOUCH_ROUNDS}" \
      DONOR_CPU_MODE="${DONOR_CPU_MODE}" \
      DONOR_FIXED_CPU="${DONOR_FIXED_CPU}" \
      ROUNDS="${ROUNDS}" \
      SETTLE_SECS="${SETTLE_SECS}" \
      TARGET_PHOTOS="${TARGET_PHOTOS}" \
      PHOTO_POLL_EVERY_ATTEMPTS="${PHOTO_POLL_EVERY_ATTEMPTS}" \
      SHOOT_TIMEOUT_SECS="${SHOOT_TIMEOUT_SECS}" \
      SHOOT_MODE="${SHOOT_MODE}" \
      TAP_INTERVAL_MS="${TAP_INTERVAL_MS}" \
      POST_WAIT_SECS="${POST_WAIT_SECS}" \
      DRAIN_STABLE_SECS="${DRAIN_STABLE_SECS}" \
      DRAIN_TIMEOUT_SECS="${DRAIN_TIMEOUT_SECS}" \
      CAMERA_COMPONENT="${CAMERA_COMPONENT}" \
      SHUTTER_X="${SHUTTER_X}" \
      SHUTTER_Y="${SHUTTER_Y}" \
      PREPARE_DEVICE="${PREPARE_DEVICE}" \
      GROUP_CLEANUP="${GROUP_CLEANUP}" \
      RESTART_BROKER_ON_START="${RESTART_BROKER_ON_START}" \
      RESTART_BROKER_BETWEEN_GROUPS="${RESTART_BROKER_BETWEEN_GROUPS}" \
      RESTART_BROKER_BETWEEN_ROUNDS="${RESTART_BROKER_BETWEEN_ROUNDS}" \
      ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS="${ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS}" \
      CLEAN_PHOTOS_BETWEEN_GROUPS="${CLEAN_PHOTOS_BETWEEN_GROUPS}" \
      CLEAN_PHOTOS_BETWEEN_ROUNDS="${CLEAN_PHOTOS_BETWEEN_ROUNDS}" \
      CLEAN_PHOTOS_ON_EXIT="${CLEAN_PHOTOS_ON_EXIT}" \
      FINAL_PHOTO_CLEANUP_WAIT_SECS="${FINAL_PHOTO_CLEANUP_WAIT_SECS}" \
      DISMISS_USB_DIALOG="${DISMISS_USB_DIALOG}" \
      DISMISS_USB_DIALOG_TAPS="${DISMISS_USB_DIALOG_TAPS}" \
      DUMP_STATE_BETWEEN_STEPS="${DUMP_STATE_BETWEEN_STEPS}" \
      "${BASH_SOURCE[0]}"
    cleanup_phone_photos
  done
  exit 0
fi

PHONE_SCRIPT="$(mktemp)"
ENV_SCRIPT="$(mktemp)"
cat >"${PHONE_SCRIPT}" <<'EOS'
#!/system/bin/sh
set -eu

PHONE_DIR="${PHONE_DIR:-/data/local/tmp/md_camera}"
VARIANT_LABEL="${VARIANT_LABEL:-current}"
PRESSURE_APPS="${PRESSURE_APPS:-0 2 4 6}"
PRESSURE_MB_PER_APP="${PRESSURE_MB_PER_APP:-512}"
PRESSURE_SECONDS="${PRESSURE_SECONDS:-240}"
PRESSURE_TOUCH_MS="${PRESSURE_TOUCH_MS:-1000}"
BACKGROUND_MODE="${BACKGROUND_MODE:-donor}"
DONOR_MODE="${DONOR_MODE:-auto}"
DONOR_CHUNKS="${DONOR_CHUNKS:-0}"
DONOR_CHUNK_KB="${DONOR_CHUNK_KB:-1024}"
DONOR_TOUCH_ROUNDS="${DONOR_TOUCH_ROUNDS:-1}"
DONOR_READY_TIMEOUT_SECS="${DONOR_READY_TIMEOUT_SECS:-60}"
DONOR_CPU_MODE="${DONOR_CPU_MODE:-spread}"
DONOR_FIXED_CPU="${DONOR_FIXED_CPU:-0}"
ROUNDS="${ROUNDS:-3}"
SETTLE_SECS="${SETTLE_SECS:-30}"
SHOT_SECONDS="${SHOT_SECONDS:-10}"
TARGET_PHOTOS="${TARGET_PHOTOS:-30}"
PHOTO_POLL_EVERY_ATTEMPTS="${PHOTO_POLL_EVERY_ATTEMPTS:-1}"
SHOOT_TIMEOUT_SECS="${SHOOT_TIMEOUT_SECS:-90}"
SHOOT_MODE="${SHOOT_MODE:-tap}"
TAP_INTERVAL_MS="${TAP_INTERVAL_MS:-120}"
POST_WAIT_SECS="${POST_WAIT_SECS:-5}"
DRAIN_STABLE_SECS="${DRAIN_STABLE_SECS:-2}"
DRAIN_TIMEOUT_SECS="${DRAIN_TIMEOUT_SECS:-30}"
CAMERA_COMPONENT="${CAMERA_COMPONENT:-com.oplus.camera/.Camera}"
SHUTTER_X="${SHUTTER_X:-350}"
SHUTTER_Y="${SHUTTER_Y:-636}"
PHOTO_DIR="${PHOTO_DIR:-/sdcard/DCIM/Camera}"
PREPARE_DEVICE="${PREPARE_DEVICE:-1}"
GROUP_CLEANUP="${GROUP_CLEANUP:-1}"
ALLOW_PARTIAL_CSV="${ALLOW_PARTIAL_CSV:-0}"
CLEAN_PROCESSES_BETWEEN_ROUNDS="${CLEAN_PROCESSES_BETWEEN_ROUNDS:-1}"
ROUND_CLEANUP_SETTLE_SECS="${ROUND_CLEANUP_SETTLE_SECS:-5}"
RESTART_BROKER_BETWEEN_GROUPS="${RESTART_BROKER_BETWEEN_GROUPS:-0}"
RESTART_BROKER_BETWEEN_ROUNDS="${RESTART_BROKER_BETWEEN_ROUNDS:-0}"
RESTART_BROKER_ON_START="${RESTART_BROKER_ON_START:-1}"
ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS="${ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS:-1}"
CLEAN_PHOTOS_BETWEEN_GROUPS="${CLEAN_PHOTOS_BETWEEN_GROUPS:-1}"
CLEAN_PHOTOS_BETWEEN_ROUNDS="${CLEAN_PHOTOS_BETWEEN_ROUNDS:-1}"
CLEAN_PHOTOS_ON_EXIT="${CLEAN_PHOTOS_ON_EXIT:-1}"
FINAL_PHOTO_CLEANUP_WAIT_SECS="${FINAL_PHOTO_CLEANUP_WAIT_SECS:-5}"
DUMP_STATE_BETWEEN_STEPS="${DUMP_STATE_BETWEEN_STEPS:-1}"
DISMISS_USB_DIALOG="${DISMISS_USB_DIALOG:-1}"
DISMISS_USB_DIALOG_TAPS="${DISMISS_USB_DIALOG_TAPS:-636,2360 636,2180 950,2360 950,2180}"
TRACE_PHONE_SCRIPT="${TRACE_PHONE_SCRIPT:-0}"
CAMERA_PKG="${CAMERA_PKG:-com.oplus.camera}"
MD_CAMERA_MODULE_DIR="${MD_CAMERA_MODULE_DIR:-/data/adb/modules/md-camera-scudo}"
BROKER_LOG_DIR="${BROKER_LOG_DIR:-/data/local/tmp/md}"
BROKER_LOG="${BROKER_LOG:-camera_scudo_broker.log}"
TRACE_FILE="${TRACE_FILE:-/data/local/tmp/md_camera_scudo_trace.log}"
WRAP_LOG="${WRAP_LOG:-/data/local/tmp/md_camera_scudo_wrap.log}"
ALLOC_STATS_FILE="${ALLOC_STATS_FILE:-/data/local/tmp/md_camera_scudo_alloc_stats.log}"
APP_CACHE_DIR="${APP_CACHE_DIR:-/data/data/${CAMERA_PKG}/cache}"
APP_TRACE_FILE="${APP_TRACE_FILE:-${APP_CACHE_DIR}/md_camera_scudo_trace.log}"
APP_ALLOC_STATS_FILE="${APP_ALLOC_STATS_FILE:-${APP_CACHE_DIR}/md_camera_scudo_alloc_stats.log}"
GROUP_LOG_ROOT="${GROUP_LOG_ROOT:-${PHONE_DIR}/group_logs}"
PHOTO_MARKER="${PHOTO_MARKER:-${PHONE_DIR}/.photo_cleanup_marker}"
PHOTO_SNAPSHOT="${PHOTO_SNAPSHOT:-${PHONE_DIR}/.photo_cleanup_snapshot}"
PHOTO_DELETE_LIST="${PHOTO_DELETE_LIST:-${PHONE_DIR}/.photo_cleanup_delete_list}"

cd "$PHONE_DIR" || exit 1

trace_step() {
  [ "$TRACE_PHONE_SCRIPT" = "1" ] || return 0
  echo "CAMERA_TRACE $*"
}

kill_pressure() {
  trace_step "kill_pressure start"
  pids="$(pidof md_mem_pressure_app 2>/dev/null || true)"
  for p in $pids; do
    kill -TERM "$p" || true
  done
  pids="$(pidof md_camera_arena_donor 2>/dev/null || true)"
  for p in $pids; do
    kill -TERM "$p" || true
  done
  sleep 1
  pids="$(pidof md_mem_pressure_app 2>/dev/null || true)"
  for p in $pids; do
    kill -KILL "$p" || true
  done
  pids="$(pidof md_camera_arena_donor 2>/dev/null || true)"
  for p in $pids; do
    kill -KILL "$p" || true
  done
  trace_step "kill_pressure done"
}

mem_avail_kb() {
  awk '/MemAvailable:/ {print $2}' /proc/meminfo
}

camera_pid() {
  pidof com.oplus.camera 2>/dev/null | awk '{print $1}'
}

wait_camera_stopped() {
  waited=0
  while [ "$waited" -lt 10 ]; do
    [ -z "$(camera_pid)" ] && return 0
    sleep 1
    waited=$((waited + 1))
  done
  return 0
}

force_stop_camera() {
  am force-stop "$CAMERA_PKG" 2>/dev/null || true
  wait_camera_stopped
  pids="$(pidof "$CAMERA_PKG" 2>/dev/null || true)"
  for p in $pids; do
    kill -TERM "$p" 2>/dev/null || true
  done
  sleep 1
  wait_camera_stopped
  pids="$(pidof "$CAMERA_PKG" 2>/dev/null || true)"
  for p in $pids; do
    kill -KILL "$p" 2>/dev/null || true
  done
  wait_camera_stopped
}

camera_rollup_value_kb() {
  pid="$1"
  key="$2"
  [ -n "$pid" ] || {
    echo 0
    return
  }
  awk -v k="$key" '$1 == k":" {print $2; found=1; exit} END {if (!found) print 0}' \
    "/proc/$pid/smaps_rollup" 2>/dev/null || echo 0
}

camera_maps_pss_kb() {
  pid="$1"
  pattern="$2"
  [ -n "$pid" ] || {
    echo 0
    return
  }
  awk -v pat="$pattern" '
    /^[[:xdigit:]]+-[[:xdigit:]]+/ {
      match_map = index($0, pat) != 0
      next
    }
    match_map && $1 == "Pss:" {
      sum += $2
      match_map = 0
    }
    END {print sum + 0}
  ' "/proc/$pid/smaps" 2>/dev/null || echo 0
}

camera_maps_count() {
  pid="$1"
  pattern="$2"
  [ -n "$pid" ] || {
    echo 0
    return
  }
  awk -v pat="$pattern" '
    /^[[:xdigit:]]+-[[:xdigit:]]+/ && index($0, pat) != 0 {count++}
    END {print count + 0}
  ' "/proc/$pid/smaps" 2>/dev/null || echo 0
}

photo_count() {
  find "$PHOTO_DIR" -maxdepth 1 -type f \
    \( -name '*.jpg' -o -name '*.jpeg' -o -name '*.heic' \
       -o -name '*.JPG' -o -name '*.JPEG' -o -name '*.HEIC' \) \
    2>/dev/null | wc -l | tr -d ' '
}

list_photo_files() {
  find "$PHOTO_DIR" -maxdepth 1 -type f \
    \( -name '*.jpg' -o -name '*.jpeg' -o -name '*.heic' \
       -o -name '*.JPG' -o -name '*.JPEG' -o -name '*.HEIC' \) \
    2>/dev/null
}

dump_runtime_state() {
  [ "$DUMP_STATE_BETWEEN_STEPS" = "1" ] || return 0
  label="$1"
  cam_pid="$(camera_pid)"
  broker_pids="$(pidof memory_delegation_broker 2>/dev/null || true)"
  donor_pids="$(pidof md_camera_arena_donor 2>/dev/null || true)"
  pressure_pids="$(pidof md_mem_pressure_app 2>/dev/null || true)"
  photos="$(photo_count)"
  arena_maps="$(camera_maps_count "$cam_pid" "scudo_arena_")"
  secondary_maps="$(camera_maps_count "$cam_pid" "scudo:secondary")"
  arena_pss="$(camera_maps_pss_kb "$cam_pid" "scudo_arena_")"
  secondary_pss="$(camera_maps_pss_kb "$cam_pid" "scudo:secondary")"
  broker_ready=0
  if grep -q "ready num_cores=" "${BROKER_LOG_DIR}/${BROKER_LOG}" 2>/dev/null; then
    broker_ready=1
  fi
  echo "CAMERA_STATE label=$label broker_pids='${broker_pids}' broker_ready=$broker_ready camera_pid=${cam_pid:-0} donor_pids='${donor_pids}' pressure_pids='${pressure_pids}' photos=$photos arena_maps=$arena_maps secondary_maps=$secondary_maps arena_pss_kb=$arena_pss secondary_pss_kb=$secondary_pss"
}

dismiss_blocking_dialogs() {
  [ "$DISMISS_USB_DIALOG" = "1" ] || return 0
  cmd statusbar collapse 2>/dev/null || true
  wm dismiss-keyguard 2>/dev/null || true
  for xy in $DISMISS_USB_DIALOG_TAPS; do
    x="${xy%,*}"
    y="${xy#*,}"
    input tap "$x" "$y" 2>/dev/null || true
    sleep 0.2
  done
  input keyevent KEYCODE_ESCAPE 2>/dev/null || true
  cmd statusbar collapse 2>/dev/null || true
}

reset_photo_marker() {
  mkdir -p "$PHONE_DIR" 2>/dev/null || true
  rm -f "$PHOTO_MARKER" 2>/dev/null || true
  : >"$PHOTO_MARKER"
  list_photo_files |
  while IFS= read -r photo; do
    echo "${photo##*/}"
  done >"$PHOTO_SNAPSHOT" 2>/dev/null || true
}

delete_photos_since_marker() {
  [ "$CLEAN_PHOTOS_BETWEEN_GROUPS" = "1" ] || return 0
  [ -f "$PHOTO_MARKER" ] || return 0
  : >"$PHOTO_DELETE_LIST" 2>/dev/null || true
  list_photo_files |
  while IFS= read -r photo; do
    base="${photo##*/}"
    if [ -f "$PHOTO_SNAPSHOT" ] && grep -Fxq "$base" "$PHOTO_SNAPSHOT" 2>/dev/null; then
      if [ "$photo" -nt "$PHOTO_MARKER" ]; then
        echo "$photo"
      fi
    else
      echo "$photo"
    fi
  done | sort -u >"$PHOTO_DELETE_LIST" 2>/dev/null || true

  deleted=0
  while IFS= read -r photo; do
    [ -n "$photo" ] || continue
    rm -f "$photo" 2>/dev/null || true
    deleted=$((deleted + 1))
  done <"$PHOTO_DELETE_LIST" 2>/dev/null || true
  rm -f "$PHOTO_DELETE_LIST" 2>/dev/null || true
  echo "CAMERA_NOTE deleted_photos_since_marker=$deleted"
}

archive_one_log() {
  src="$1"
  dst_dir="$2"
  name="$3"
  [ -e "$src" ] || return 0
  cp -p "$src" "$dst_dir/$name" 2>/dev/null || true
}

archive_arena_logs() {
  [ "$ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS" = "1" ] || return 0
  label="$1"
  stamp="$(date +%Y%m%d-%H%M%S 2>/dev/null || date +%s)"
  dst="${GROUP_LOG_ROOT}/${stamp}-${label}"
  mkdir -p "$dst" 2>/dev/null || return 0
  archive_one_log "${BROKER_LOG_DIR}/${BROKER_LOG}" "$dst" "broker.log"
  archive_one_log "$TRACE_FILE" "$dst" "trace.log"
  archive_one_log "$WRAP_LOG" "$dst" "wrap.log"
  archive_one_log "$ALLOC_STATS_FILE" "$dst" "alloc_stats.log"
  archive_one_log "$APP_TRACE_FILE" "$dst" "app_trace.log"
  archive_one_log "$APP_ALLOC_STATS_FILE" "$dst" "app_alloc_stats.log"
  echo "CAMERA_NOTE archived_arena_logs=$dst"
}

clear_arena_logs() {
  mkdir -p "$BROKER_LOG_DIR" "$APP_CACHE_DIR" 2>/dev/null || true
  rm -f "$WRAP_LOG" "$TRACE_FILE" "$ALLOC_STATS_FILE" "$APP_TRACE_FILE" "$APP_ALLOC_STATS_FILE" 2>/dev/null || true
  : >"$TRACE_FILE" 2>/dev/null || true
  : >"$ALLOC_STATS_FILE" 2>/dev/null || true
  : >"$APP_TRACE_FILE" 2>/dev/null || true
  : >"$APP_ALLOC_STATS_FILE" 2>/dev/null || true
  chmod 666 "$TRACE_FILE" "$ALLOC_STATS_FILE" 2>/dev/null || true
  chmod 660 "$APP_TRACE_FILE" "$APP_ALLOC_STATS_FILE" 2>/dev/null || true
}

restart_camera_broker() {
  trace_step "restart_camera_broker start"
  [ "$(resolved_donor_mode)" = "delegated" ] || return 0
  broker="${MD_CAMERA_MODULE_DIR}/system/bin/memory_delegation_broker"
  [ -x "$broker" ] || {
    echo "CAMERA_NOTE broker_restart_skipped_missing=$broker"
    return 0
  }
  pids="$(pidof memory_delegation_broker 2>/dev/null || true)"
  trace_step "restart_camera_broker term_pids=$pids"
  for p in $pids; do
    kill -TERM "$p" || true
  done
  sleep 1
  pids="$(pidof memory_delegation_broker 2>/dev/null || true)"
  trace_step "restart_camera_broker kill_pids=$pids"
  for p in $pids; do
    kill -KILL "$p" || true
  done
  mkdir -p "$BROKER_LOG_DIR"
  rm -f "${BROKER_LOG_DIR}/${BROKER_LOG}"
  cd "$BROKER_LOG_DIR" || return 0
  "$broker" --trace --ashmem >"$BROKER_LOG" 2>&1 &
  broker_pid="$!"
  trace_step "restart_camera_broker launched pid=$broker_pid"
  cd "$PHONE_DIR" || true

  waited=0
  while [ "$waited" -lt 20 ]; do
    if grep -q "ready num_cores=" "${BROKER_LOG_DIR}/${BROKER_LOG}" 2>/dev/null; then
      echo "CAMERA_NOTE broker_restarted pid=$broker_pid ready=1"
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  echo "CAMERA_NOTE broker_restarted pid=$broker_pid ready=0"
}

prepare_group_state() {
  apps="$1"
  [ "$GROUP_CLEANUP" = "1" ] || return 0
  echo "CAMERA_NOTE prepare_group apps=$apps"
  kill_pressure
  force_stop_camera
  delete_photos_since_marker
  archive_arena_logs "before-apps${apps}"
  if [ "$RESTART_BROKER_BETWEEN_GROUPS" = "1" ]; then
    restart_camera_broker
    force_stop_camera
  fi
  clear_arena_logs
  reset_photo_marker
  dump_runtime_state "after-prepare-group-apps${apps}"
}

finish_run_state() {
  [ "$GROUP_CLEANUP" = "1" ] || return 0
  kill_pressure
  force_stop_camera
  if [ "$CLEAN_PHOTOS_ON_EXIT" = "1" ]; then
    sleep "$FINAL_PHOTO_CLEANUP_WAIT_SECS" 2>/dev/null || true
    delete_photos_since_marker
  fi
  archive_arena_logs "final"
}

trap finish_run_state EXIT

prepare_device() {
  [ "$PREPARE_DEVICE" = "1" ] || return 0
  input keyevent KEYCODE_WAKEUP || true
  wm dismiss-keyguard 2>/dev/null || true
  dismiss_blocking_dialogs
  am force-stop com.oplus.ota 2>/dev/null || true
  cmd statusbar collapse 2>/dev/null || true
  settings put system screen_brightness_mode 0 2>/dev/null || true
  settings put system screen_brightness 120 2>/dev/null || true
  settings put system screen_off_timeout 600000 2>/dev/null || true
  settings put global window_animation_scale 0 2>/dev/null || true
  settings put global transition_animation_scale 0 2>/dev/null || true
  settings put global animator_duration_scale 0 2>/dev/null || true
  svc power stayon true 2>/dev/null || true
}

camera_focus_ok() {
  dumpsys window 2>/dev/null |
    grep 'mCurrentFocus=' |
    grep -q 'com.oplus.camera'
}

current_focus_line() {
  dumpsys window 2>/dev/null | grep 'mCurrentFocus=' | tail -n 1 || true
}

recover_camera_focus() {
  tries="${1:-8}"
  i=0
  while [ "$i" -lt "$tries" ]; do
    camera_focus_ok && return 0
    cmd statusbar collapse 2>/dev/null || true
    input keyevent KEYCODE_BACK || true
    am force-stop com.oplus.ota 2>/dev/null || true
    sleep 1
    i=$((i + 1))
  done
  camera_focus_ok
}

wait_camera_focus() {
  tries="${1:-12}"
  i=0
  while [ "$i" -lt "$tries" ]; do
    camera_focus_ok && return 0
    cmd statusbar collapse 2>/dev/null || true
    am force-stop com.oplus.ota 2>/dev/null || true
    sleep 1
    i=$((i + 1))
  done
  camera_focus_ok
}

start_camera() {
  input keyevent KEYCODE_WAKEUP || true
  sleep 0.5
  input swipe 636 1100 636 200 300 || true
  wm dismiss-keyguard 2>/dev/null || true
  dismiss_blocking_dialogs
  cmd statusbar collapse 2>/dev/null || true
  am force-stop com.oplus.ota 2>/dev/null || true
  force_stop_camera
  sleep 1
  dismiss_blocking_dialogs
  am start -n "$CAMERA_COMPONENT" >/dev/null
  sleep 1
  dismiss_blocking_dialogs
  wait_camera_focus 10 || true
  sleep 3
}

start_pressure() {
  n="$1"
  mode="$(resolved_donor_mode)"
  rm -f pressure_"${VARIANT_LABEL}-apps${n}"-p*.log
  i=0
  while [ "$i" -lt "$n" ]; do
    label="${VARIANT_LABEL}-apps${n}-p${i}"
    if [ "$mode" = "delegated" ]; then
      allocator_env="SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=0"
    else
      allocator_env="SCUDO_SHARED_ARENA_DISABLE=1 SCUDO_SHARED_ARENA_TRACE=0"
    fi
    env $allocator_env \
    MD_PRESSURE_LABEL="$label" \
    MD_PRESSURE_MB="$PRESSURE_MB_PER_APP" \
    MD_PRESSURE_SECONDS="$PRESSURE_SECONDS" \
    MD_PRESSURE_TOUCH_MS="$PRESSURE_TOUCH_MS" \
      ./md_mem_pressure_app >"pressure_${label}.log" 2>&1 &
    i=$((i + 1))
  done

  waited=0
  while [ "$waited" -lt 20 ]; do
    ready=$(grep -h "MD_PRESSURE_READY" pressure_"${VARIANT_LABEL}-apps${n}"-p*.log 2>/dev/null | wc -l | tr -d ' ')
    [ "$ready" -ge "$n" ] && break
    sleep 1
    waited=$((waited + 1))
  done
}

resolved_donor_mode() {
  case "$DONOR_MODE" in
    delegated|sharedarena|mem-delegated)
      echo delegated
      ;;
    default|baseline|scudo-default)
      echo default
      ;;
    auto)
      case "$VARIANT_LABEL" in
        *delegated*|*shared*|*arena*)
          echo delegated
          ;;
        *)
          echo default
          ;;
      esac
      ;;
    *)
      echo "unknown DONOR_MODE=$DONOR_MODE" >&2
      return 2
      ;;
  esac
}

start_donors() {
  n="$1"
  mode="$(resolved_donor_mode)"
  rm -f donor_"${VARIANT_LABEL}-apps${n}"-p*.log
  cpus="$(getconf _NPROCESSORS_CONF 2>/dev/null || echo 8)"
  [ -n "$cpus" ] || cpus=8
  [ "$cpus" -gt 0 ] 2>/dev/null || cpus=8

  chunks="$DONOR_CHUNKS"
  if [ "$chunks" -le 0 ] 2>/dev/null; then
    chunks=$((PRESSURE_MB_PER_APP * 1024 / DONOR_CHUNK_KB))
    [ "$chunks" -gt 0 ] || chunks=1
  fi

  i=0
  while [ "$i" -lt "$n" ]; do
    label="${VARIANT_LABEL}-apps${n}-p${i}"
    if [ "$DONOR_CPU_MODE" = "fixed" ]; then
      cpu="$DONOR_FIXED_CPU"
    else
      cpu=$((i % cpus))
    fi
    if [ "$mode" = "delegated" ]; then
      allocator_env="SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=0"
    else
      allocator_env="SCUDO_SHARED_ARENA_DISABLE=1 SCUDO_SHARED_ARENA_TRACE=0"
    fi
    env $allocator_env \
      MD_CAMERA_DONOR_CPU="$cpu" \
      MD_CAMERA_DONOR_CHUNKS="$chunks" \
      MD_CAMERA_DONOR_CHUNK_KB="$DONOR_CHUNK_KB" \
      MD_CAMERA_DONOR_HOLD_SECS="$PRESSURE_SECONDS" \
      MD_CAMERA_DONOR_TOUCH_ROUNDS="$DONOR_TOUCH_ROUNDS" \
      ./md_camera_arena_donor >"donor_${label}.log" 2>&1 &
    i=$((i + 1))
  done

  waited=0
  ready=0
  while [ "$waited" -lt "$DONOR_READY_TIMEOUT_SECS" ]; do
    ready=$(grep -h "DONOR_READY" donor_"${VARIANT_LABEL}-apps${n}"-p*.log 2>/dev/null | wc -l | tr -d ' ')
    [ "$ready" -ge "$n" ] && break
    sleep 1
    waited=$((waited + 1))
  done
  if [ "$ready" -lt "$n" ]; then
    echo "CAMERA_NOTE donor_ready_timeout apps=$n ready=$ready expected=$n waited_s=$waited"
    for f in donor_"${VARIANT_LABEL}-apps${n}"-p*.log; do
      [ -f "$f" ] || continue
      echo "CAMERA_NOTE donor_log_tail file=$f"
      tail -40 "$f" 2>/dev/null || true
    done
    return 1
  fi
}

start_background() {
  n="$1"
  case "$BACKGROUND_MODE" in
    none)
      ;;
    pressure)
      start_pressure "$n"
      ;;
    donor)
      start_donors "$n"
      ;;
    both)
      start_pressure "$n"
      start_donors "$n"
      ;;
    *)
      echo "unknown BACKGROUND_MODE=$BACKGROUND_MODE" >&2
      return 2
      ;;
  esac
}

sum_donor_field() {
  n="$1"
  key="$2"
  grep -h "DONOR_STATS phase=after_free" donor_"${VARIANT_LABEL}-apps${n}"-p*.log 2>/dev/null |
    awk -v k="$key" '{
      for (i = 1; i <= NF; i++) {
        split($i, a, "=")
        if (a[1] == k)
          sum += a[2]
      }
    } END {print sum + 0}'
}

sum_donor_ready_field() {
  n="$1"
  key="$2"
  grep -h "DONOR_READY" donor_"${VARIANT_LABEL}-apps${n}"-p*.log 2>/dev/null |
    awk -v k="$key" '{
      for (i = 1; i <= NF; i++) {
        split($i, a, "=")
        if (a[1] == k)
          sum += a[2]
      }
    } END {print sum + 0}'
}

donor_ready_count() {
  n="$1"
  grep -h "DONOR_READY" donor_"${VARIANT_LABEL}-apps${n}"-p*.log 2>/dev/null | wc -l | tr -d ' '
}

perform_shutter_action() {
  case "$SHOOT_MODE" in
    tap)
      input tap "$SHUTTER_X" "$SHUTTER_Y"
      ;;
    keyevent|camera_key|camera-key)
      input keyevent KEYCODE_CAMERA
      ;;
    *)
      input tap "$SHUTTER_X" "$SHUTTER_Y"
      ;;
  esac
}

tap_loop() {
  end_ms=$(( $(date +%s%3N) + SHOT_SECONDS * 1000 ))
  attempts=0
  while [ "$(date +%s%3N)" -lt "$end_ms" ]; do
    perform_shutter_action
    attempts=$((attempts + 1))
    sleep "$(printf '0.%03d' "$TAP_INTERVAL_MS")"
  done
  echo "$attempts"
}

long_press_shutter() {
  duration_ms=$((SHOT_SECONDS * 1000))
  input swipe "$SHUTTER_X" "$SHUTTER_Y" "$SHUTTER_X" "$SHUTTER_Y" "$duration_ms"
  echo 1
}

shoot_until_target() {
  before="$1"
  target="$2"
  timeout_s="$3"
  poll_every="${PHOTO_POLL_EVERY_ATTEMPTS:-1}"
  [ "$poll_every" -gt 0 ] 2>/dev/null || poll_every=1
  end_ms=$(( $(date +%s%3N) + timeout_s * 1000 ))
  attempts=0
  current="$before"
  while [ "$(date +%s%3N)" -lt "$end_ms" ]; do
    perform_shutter_action
    attempts=$((attempts + 1))
    if [ $((attempts % poll_every)) -eq 0 ]; then
      current="$(photo_count)"
      made=$((current - before))
      [ "$made" -ge "$target" ] && break
    fi
    sleep "$(printf '0.%03d' "$TAP_INTERVAL_MS")"
  done
  current="$(photo_count)"
  made=$((current - before))
  [ "$made" -lt 0 ] && made=0
  met=0
  [ "$made" -ge "$target" ] && met=1
  echo "$attempts $made $met"
}

wait_drain_stable() {
  start_count="$1"
  timeout_s="$2"
  stable_s="$3"
  end_ms=$(( $(date +%s%3N) + timeout_s * 1000 ))
  stable_start=0
  last="$(photo_count)"
  while [ "$(date +%s%3N)" -lt "$end_ms" ]; do
    sleep 1
    cur="$(photo_count)"
    if [ "$cur" = "$last" ]; then
      if [ "$stable_start" -eq 0 ]; then
        stable_start="$(date +%s%3N)"
      fi
      now="$(date +%s%3N)"
      if [ $((now - stable_start)) -ge $((stable_s * 1000)) ]; then
        break
      fi
    else
      stable_start=0
      last="$cur"
    fi
  done
  final="$(photo_count)"
  made=$((final - start_count))
  [ "$made" -lt 0 ] && made=0
  echo "$made"
}

shoot_once() {
  case "$SHOOT_MODE" in
    tap)
      tap_loop
      ;;
    keyevent|camera_key|camera-key)
      tap_loop
      ;;
    longpress|long_press)
      long_press_shutter
      ;;
    *)
      echo "unknown SHOOT_MODE=$SHOOT_MODE" >&2
      return 2
      ;;
  esac
}

echo "CAMERA_NOTE variant=$VARIANT_LABEL strict_baseline_sharedarena_toggle=unavailable_for_system_camera"
echo "CAMERA_NOTE component=$CAMERA_COMPONENT shutter=${SHUTTER_X},${SHUTTER_Y} shoot_mode=$SHOOT_MODE photo_dir=$PHOTO_DIR"
echo "CAMERA_NOTE background_mode=$BACKGROUND_MODE donor_mode=$(resolved_donor_mode) target_photos=$TARGET_PHOTOS tap_interval_ms=$TAP_INTERVAL_MS photo_poll_every_attempts=$PHOTO_POLL_EVERY_ATTEMPTS drain_stable_secs=$DRAIN_STABLE_SECS"
echo "CAMERA_NOTE pressure_apps='$PRESSURE_APPS' rounds=$ROUNDS"
echo "CAMERA_NOTE group_cleanup=$GROUP_CLEANUP clean_processes_between_rounds=$CLEAN_PROCESSES_BETWEEN_ROUNDS round_cleanup_settle_secs=$ROUND_CLEANUP_SETTLE_SECS restart_broker_between_groups=$RESTART_BROKER_BETWEEN_GROUPS restart_broker_between_rounds=$RESTART_BROKER_BETWEEN_ROUNDS archive_arena_logs_between_groups=$ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS clean_photos_between_groups=$CLEAN_PHOTOS_BETWEEN_GROUPS clean_photos_between_rounds=$CLEAN_PHOTOS_BETWEEN_ROUNDS group_log_root=$GROUP_LOG_ROOT"
echo "CAMERA_NOTE dismiss_usb_dialog=$DISMISS_USB_DIALOG dismiss_usb_dialog_taps='$DISMISS_USB_DIALOG_TAPS'"
echo "CSV variant,shoot_mode,background_mode,donor_mode,pressure_apps,round,pressure_mb_total,target_photos,photo_target_met,focus_ok,photos,drain_photos,elapsed_s,photos_per_s,drain_s,attempts,donor_ready,donor_arena_allocs,donor_arena_free_reusable_kb,donor_arena_donated_kb,donor_secondary_pss_kb,mem_avail_before_kb,mem_avail_after_kb,camera_pid,rss_before_kb,rss_after_kb,pss_before_kb,pss_after_kb,private_dirty_before_kb,private_dirty_after_kb,scudo_secondary_pss_before_kb,scudo_secondary_pss_after_kb,arena_pss_before_kb,arena_pss_after_kb,arena_map_count_before,arena_map_count_after"

kill_pressure
trace_step "main after kill_pressure"
prepare_device
trace_step "main after prepare_device"
if [ "$RESTART_BROKER_ON_START" = "1" ]; then
  restart_camera_broker
  trace_step "main after restart_camera_broker"
fi
clear_arena_logs
trace_step "main after clear_arena_logs"
reset_photo_marker
trace_step "main after reset_photo_marker"

for apps in $PRESSURE_APPS; do
  prepare_group_state "$apps"
  round=1
  while [ "$round" -le "$ROUNDS" ]; do
    kill_pressure
    if [ "$RESTART_BROKER_BETWEEN_ROUNDS" = "1" ]; then
      force_stop_camera
      archive_arena_logs "before-apps${apps}-round${round}-broker-restart"
      restart_camera_broker
      force_stop_camera
      clear_arena_logs
      dump_runtime_state "after-broker-restart-apps${apps}-round${round}"
    fi
    start_background "$apps"
    dump_runtime_state "after-start-background-apps${apps}-round${round}"
    start_camera
    wait_camera_focus 5 || true
    dump_runtime_state "after-start-camera-apps${apps}-round${round}"
    focus_ok=0
    camera_focus_ok && focus_ok=1
    if [ "$focus_ok" != "1" ]; then
      echo "CAMERA_NOTE focus_not_camera apps=$apps round=$round focus='$(current_focus_line)'"
    fi
    cam_pid="$(camera_pid)"
    mem_before="$(mem_avail_kb)"
    rss_before="$(camera_rollup_value_kb "$cam_pid" Rss)"
    pss_before="$(camera_rollup_value_kb "$cam_pid" Pss)"
    private_dirty_before="$(camera_rollup_value_kb "$cam_pid" Private_Dirty)"
    secondary_pss_before="$(camera_maps_pss_kb "$cam_pid" "scudo:secondary")"
    arena_pss_before="$(camera_maps_pss_kb "$cam_pid" "scudo_arena_")"
    arena_map_count_before="$(camera_maps_count "$cam_pid" "scudo_arena_")"
    sleep "$SETTLE_SECS"

    before="$(photo_count)"
    start_ms="$(date +%s%3N)"
    target_met=0
    drain_photos=0
    if [ "$TARGET_PHOTOS" -gt 0 ] 2>/dev/null; then
      shot_result="$(shoot_until_target "$before" "$TARGET_PHOTOS" "$SHOOT_TIMEOUT_SECS")"
      attempts="$(echo "$shot_result" | awk '{print $1}')"
      photos="$(echo "$shot_result" | awk '{print $2}')"
      target_met="$(echo "$shot_result" | awk '{print $3}')"
    else
      attempts="$(shoot_once)"
      after_shot="$(photo_count)"
      photos=$((after_shot - before))
      [ "$photos" -lt 0 ] && photos=0
      target_met=1
    fi
    end_ms="$(date +%s%3N)"
    drain_start_ms="$(date +%s%3N)"
    if [ "$DRAIN_TIMEOUT_SECS" -gt 0 ] 2>/dev/null; then
      drain_photos="$(wait_drain_stable "$before" "$DRAIN_TIMEOUT_SECS" "$DRAIN_STABLE_SECS")"
    else
      sleep "$POST_WAIT_SECS"
      after_drain="$(photo_count)"
      drain_photos=$((after_drain - before))
      [ "$drain_photos" -lt 0 ] && drain_photos=0
    fi
    drain_end_ms="$(date +%s%3N)"
    sleep "$POST_WAIT_SECS"
    mem_after="$(mem_avail_kb)"
    rss_after="$(camera_rollup_value_kb "$cam_pid" Rss)"
    pss_after="$(camera_rollup_value_kb "$cam_pid" Pss)"
    private_dirty_after="$(camera_rollup_value_kb "$cam_pid" Private_Dirty)"
    secondary_pss_after="$(camera_maps_pss_kb "$cam_pid" "scudo:secondary")"
    arena_pss_after="$(camera_maps_pss_kb "$cam_pid" "scudo_arena_")"
    arena_map_count_after="$(camera_maps_count "$cam_pid" "scudo_arena_")"

    elapsed_ms=$((end_ms - start_ms))
    drain_ms=$((drain_end_ms - drain_start_ms))
    pressure_total=$((apps * PRESSURE_MB_PER_APP))
    pps="$(awk -v p="$photos" -v ms="$elapsed_ms" 'BEGIN { if (ms > 0) printf "%.6f", p * 1000.0 / ms; else printf "0.000000" }')"
    elapsed_s="$(awk -v ms="$elapsed_ms" 'BEGIN { printf "%.3f", ms / 1000.0 }')"
    drain_s="$(awk -v ms="$drain_ms" 'BEGIN { printf "%.3f", ms / 1000.0 }')"
    donor_ready="$(donor_ready_count "$apps")"
    donor_arena_allocs="$(sum_donor_ready_field "$apps" arena_allocs)"
    donor_arena_free_reusable_kb="$(sum_donor_field "$apps" arena_free_reusable_bytes | awk '{printf "%d", $1 / 1024}')"
    donor_arena_donated_kb="$(sum_donor_field "$apps" arena_donated_bytes | awk '{printf "%d", $1 / 1024}')"
    donor_secondary_pss_kb="$(sum_donor_field "$apps" smaps_secondary_pss_kb)"
    donor_mode="$(resolved_donor_mode)"

    echo "CSV $VARIANT_LABEL,$SHOOT_MODE,$BACKGROUND_MODE,$donor_mode,$apps,$round,$pressure_total,$TARGET_PHOTOS,$target_met,$focus_ok,$photos,$drain_photos,$elapsed_s,$pps,$drain_s,$attempts,$donor_ready,$donor_arena_allocs,$donor_arena_free_reusable_kb,$donor_arena_donated_kb,$donor_secondary_pss_kb,$mem_before,$mem_after,$cam_pid,$rss_before,$rss_after,$pss_before,$pss_after,$private_dirty_before,$private_dirty_after,$secondary_pss_before,$secondary_pss_after,$arena_pss_before,$arena_pss_after,$arena_map_count_before,$arena_map_count_after"
    if [ "$GROUP_CLEANUP" = "1" ] && [ "$CLEAN_PHOTOS_BETWEEN_ROUNDS" = "1" ]; then
      echo "CAMERA_NOTE round_cleanup_start apps=$apps round=$round"
      if [ "$CLEAN_PROCESSES_BETWEEN_ROUNDS" = "1" ]; then
        force_stop_camera
        kill_pressure
        dump_runtime_state "after-round-process-cleanup-apps${apps}-round${round}"
        sleep "$ROUND_CLEANUP_SETTLE_SECS" 2>/dev/null || true
      fi
      delete_photos_since_marker
      reset_photo_marker
      echo "CAMERA_NOTE round_cleanup_done apps=$apps round=$round"
    fi
    round=$((round + 1))
  done
done

kill_pressure
echo "CAMERA_NOTE completed_all_groups=1"
EOS
chmod 755 "${PHONE_SCRIPT}"

cleanup() {
  rm -f "${PHONE_SCRIPT}" "${ENV_SCRIPT}"
}
trap cleanup EXIT

sh_quote() {
  local s="${1//\'/\'\\\'\'}"
  printf "'%s'" "${s}"
}

write_env_var() {
  local name="$1"
  printf 'export %s=%s\n' "${name}" "$(sh_quote "${!name-}")" >>"${ENV_SCRIPT}"
}

: >"${ENV_SCRIPT}"
for name in \
  PHONE_DIR VARIANT_LABEL PRESSURE_APPS PRESSURE_MB_PER_APP PRESSURE_SECONDS \
  PRESSURE_TOUCH_MS BACKGROUND_MODE DONOR_MODE DONOR_CHUNKS DONOR_CHUNK_KB \
  DONOR_TOUCH_ROUNDS DONOR_READY_TIMEOUT_SECS DONOR_CPU_MODE DONOR_FIXED_CPU ROUNDS SETTLE_SECS \
  SHOT_SECONDS TARGET_PHOTOS PHOTO_POLL_EVERY_ATTEMPTS SHOOT_TIMEOUT_SECS \
  SHOOT_MODE TAP_INTERVAL_MS POST_WAIT_SECS DRAIN_STABLE_SECS DRAIN_TIMEOUT_SECS \
  CAMERA_COMPONENT SHUTTER_X SHUTTER_Y PREPARE_DEVICE GROUP_CLEANUP \
  ALLOW_PARTIAL_CSV CLEAN_PROCESSES_BETWEEN_ROUNDS ROUND_CLEANUP_SETTLE_SECS \
  RESTART_BROKER_BETWEEN_GROUPS RESTART_BROKER_BETWEEN_ROUNDS \
  RESTART_BROKER_ON_START ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS \
  CLEAN_PHOTOS_BETWEEN_GROUPS CLEAN_PHOTOS_BETWEEN_ROUNDS CLEAN_PHOTOS_ON_EXIT \
  FINAL_PHOTO_CLEANUP_WAIT_SECS DISMISS_USB_DIALOG DISMISS_USB_DIALOG_TAPS \
  TRACE_PHONE_SCRIPT DUMP_STATE_BETWEEN_STEPS; do
  write_env_var "${name}"
done
chmod 644 "${ENV_SCRIPT}"

mkdir -p "${REMOTE_DIR}"
cp -p "${BIN_PRESSURE}" "${REMOTE_DIR}/md_mem_pressure_app"
cp -p "${BIN_DONOR}" "${REMOTE_DIR}/md_camera_arena_donor"
cp -p "${PHONE_SCRIPT}" "${REMOTE_DIR}/run_camera_pressure_phone.sh"
cp -p "${ENV_SCRIPT}" "${REMOTE_DIR}/run_camera_pressure_env.sh"

set +e
TRACE_PHONE_SCRIPT="${TRACE_PHONE_SCRIPT:-0}"
export ADB_BIN ADB_SERIAL PHONE_DIR USE_SU CLEAR_LOGCAT REMOTE_DIR
export VARIANT_LABEL PRESSURE_APPS PRESSURE_MB_PER_APP PRESSURE_SECONDS
export PRESSURE_TOUCH_MS BACKGROUND_MODE DONOR_MODE DONOR_CHUNKS DONOR_CHUNK_KB
export DONOR_TOUCH_ROUNDS DONOR_READY_TIMEOUT_SECS DONOR_CPU_MODE DONOR_FIXED_CPU ROUNDS SETTLE_SECS
export SHOT_SECONDS TARGET_PHOTOS PHOTO_POLL_EVERY_ATTEMPTS SHOOT_TIMEOUT_SECS SHOOT_MODE TAP_INTERVAL_MS
export POST_WAIT_SECS DRAIN_STABLE_SECS DRAIN_TIMEOUT_SECS CAMERA_COMPONENT
export SHUTTER_X SHUTTER_Y PREPARE_DEVICE
export GROUP_CLEANUP ALLOW_PARTIAL_CSV CLEAN_PROCESSES_BETWEEN_ROUNDS ROUND_CLEANUP_SETTLE_SECS
export RESTART_BROKER_BETWEEN_GROUPS RESTART_BROKER_BETWEEN_ROUNDS
export RESTART_BROKER_ON_START
export ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS CLEAN_PHOTOS_BETWEEN_GROUPS
export CLEAN_PHOTOS_BETWEEN_ROUNDS CLEAN_PHOTOS_ON_EXIT FINAL_PHOTO_CLEANUP_WAIT_SECS
export DISMISS_USB_DIALOG DISMISS_USB_DIALOG_TAPS TRACE_PHONE_SCRIPT DUMP_STATE_BETWEEN_STEPS
bash -s <<'EOF' 2>&1 | tee "${OUT_LOG}"
set -euo pipefail

adb_path="${ADB_BIN}"
adb_cmd=("${adb_path}")
if [[ -n "${ADB_SERIAL}" ]]; then
  adb_cmd+=( -s "${ADB_SERIAL}" )
fi

echo "remote: adb=${adb_path} serial=${ADB_SERIAL:-<default>}"
echo "remote: source_dir=${REMOTE_DIR}"
echo "remote: phone_dir=${PHONE_DIR}"

ls -l "${REMOTE_DIR}/md_mem_pressure_app" "${REMOTE_DIR}/md_camera_arena_donor" "${REMOTE_DIR}/run_camera_pressure_phone.sh" "${REMOTE_DIR}/run_camera_pressure_env.sh"

echo "adb: wait-for-device"
"${adb_cmd[@]}" wait-for-device </dev/null
if [[ "${CLEAR_LOGCAT}" == "1" ]]; then
  echo "adb: clear logcat"
  "${adb_cmd[@]}" logcat -c </dev/null || true
fi

echo "adb: prepare ${PHONE_DIR}"
"${adb_cmd[@]}" shell "mkdir -p '${PHONE_DIR}' && chmod 755 '${PHONE_DIR}'" </dev/null
echo "adb: push helpers"
"${adb_cmd[@]}" push "${REMOTE_DIR}/md_mem_pressure_app" "${PHONE_DIR}/md_mem_pressure_app" </dev/null >/dev/null
"${adb_cmd[@]}" push "${REMOTE_DIR}/md_camera_arena_donor" "${PHONE_DIR}/md_camera_arena_donor" </dev/null >/dev/null
"${adb_cmd[@]}" push "${REMOTE_DIR}/run_camera_pressure_phone.sh" "${PHONE_DIR}/run_camera_pressure_phone.sh" </dev/null >/dev/null
"${adb_cmd[@]}" push "${REMOTE_DIR}/run_camera_pressure_env.sh" "${PHONE_DIR}/run_camera_pressure_env.sh" </dev/null >/dev/null
"${adb_cmd[@]}" shell "chmod 755 '${PHONE_DIR}/md_mem_pressure_app' '${PHONE_DIR}/md_camera_arena_donor' '${PHONE_DIR}/run_camera_pressure_phone.sh' && chmod 644 '${PHONE_DIR}/run_camera_pressure_env.sh'" </dev/null

run_cmd="cd '${PHONE_DIR}' && . ./run_camera_pressure_env.sh && sh ./run_camera_pressure_phone.sh"
echo "adb: run phone script"
if [[ "${USE_SU}" == "1" ]]; then
  "${adb_cmd[@]}" shell "su -c $(printf "%q" "${run_cmd}")" </dev/null
else
  "${adb_cmd[@]}" shell "sh -c $(printf "%q" "${run_cmd}")" </dev/null
fi
EOF
run_rc=${PIPESTATUS[0]}
set -e

awk '/^CSV / { sub(/^CSV /, ""); print }' "${OUT_LOG}" > "${OUT_CSV}"

if [[ ! -s "${OUT_CSV}" ]]; then
  echo "ERROR: no CSV rows captured; see ${OUT_LOG}" >&2
  exit "${run_rc:-1}"
fi

expected_rows=0
for _apps in ${PRESSURE_APPS}; do
  expected_rows=$((expected_rows + ROUNDS))
done
actual_rows=$(( $(wc -l <"${OUT_CSV}") - 1 ))
completed=0
if grep -q '^CAMERA_NOTE completed_all_groups=1$' "${OUT_LOG}"; then
  completed=1
fi
if [[ "${ALLOW_PARTIAL_CSV}" != "1" ]] && { [[ "${actual_rows}" -ne "${expected_rows}" ]] || [[ "${completed}" != "1" ]]; }; then
  echo "ERROR: incomplete camera run: rows=${actual_rows}/${expected_rows} completed=${completed}; see ${OUT_LOG}" >&2
  echo "csv: ${OUT_CSV}" >&2
  exit 1
fi

python3 - "${OUT_CSV}" "${OUT_PNG}" <<'PY'
import csv
import statistics
import sys
from collections import defaultdict

csv_path, png_path = sys.argv[1], sys.argv[2]
rows = []
with open(csv_path, newline="") as f:
    reader = csv.DictReader(f)
    rows = list(reader)

if not rows:
    raise SystemExit("no CSV rows to plot")

groups = defaultdict(list)
for r in rows:
    if r.get("photo_target_met") == "0" or r.get("focus_ok") == "0":
        continue
    groups[(r["variant"], int(r["pressure_apps"]))].append(float(r["photos_per_s"]))

try:
    import matplotlib.pyplot as plt
except Exception as exc:
    print(f"plot skipped: matplotlib unavailable: {exc}", file=sys.stderr)
    raise SystemExit(0)

variants = sorted({variant for variant, _ in groups})
if not variants:
    print("plot skipped: no valid rows with photo_target_met=1 and focus_ok=1", file=sys.stderr)
    raise SystemExit(0)
for variant in variants:
    xs = sorted(apps for v, apps in groups if v == variant)
    ys = [statistics.mean(groups[(variant, x)]) for x in xs]
    plt.plot(xs, ys, marker="o", label=variant)

plt.xlabel("background pressure helpers")
plt.ylabel("photos per second")
plt.title("Camera continuous shooting under memory pressure")
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()
plt.savefig(png_path, dpi=180)
print(f"plot saved: {png_path}")
PY

echo "log: ${OUT_LOG}"
echo "csv: ${OUT_CSV}"
echo "png: ${OUT_PNG}"
exit "${run_rc}"
