#!/system/bin/sh

LOG=/data/local/tmp/md_wifi_preload.log
KVER="$(uname -r)"
SYSTEM_MODULE_DIR="/system_dlkm/lib/modules/${KVER}"
VENDOR_MODULE_DIR="/vendor_dlkm/lib/modules"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"
}

wait_file() {
  file="$1"
  i=0
  while [ "$i" -lt 100 ]; do
    [ -f "$file" ] && return 0
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

module_loaded() {
  name="$1"
  grep -q "^${name}[[:space:]]" /proc/modules
}

load_module() {
  name="$1"
  path="$2"

  if module_loaded "$name"; then
    log "already loaded: ${name}"
    return 0
  fi

  if ! wait_file "$path"; then
    log "missing module after wait: ${path}"
    return 1
  fi

  out="$(insmod "$path" 2>&1)"
  rc=$?
  log "insmod ${path}: rc=${rc} ${out}"
  return "$rc"
}

{
  echo "===== md-wifi-preload start $(date '+%Y-%m-%d %H:%M:%S') ====="
  echo "kernel=${KVER}"
  echo "context=$(id)"
} >> "$LOG"

load_module libarc4 "${SYSTEM_MODULE_DIR}/kernel/lib/crypto/libarc4.ko"
load_module rfkill "${SYSTEM_MODULE_DIR}/kernel/net/rfkill/rfkill.ko"
load_module cfg80211 "${VENDOR_MODULE_DIR}/cfg80211.ko"
load_module mac80211 "${VENDOR_MODULE_DIR}/mac80211.ko"
load_module qca_cld3_peach_v2 "${VENDOR_MODULE_DIR}/qca_cld3_peach_v2.ko"

{
  echo "-- loaded wifi modules --"
  grep -E "^(libarc4|rfkill|cfg80211|mac80211|qca_cld3_peach_v2)[[:space:]]" /proc/modules || true
  echo "===== md-wifi-preload end $(date '+%Y-%m-%d %H:%M:%S') ====="
} >> "$LOG"
