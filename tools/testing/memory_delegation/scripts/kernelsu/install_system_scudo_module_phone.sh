#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Install/update the disabled-by-default md-system-scudo KernelSU module.

set -eu

TMP="${TMP:-/data/local/tmp/md-system-scudo-module-update}"
MOD="${MOD:-/data/adb/modules/md-system-scudo}"

require_file() {
  if [ ! -f "$1" ]; then
    echo "missing required file: $1" >&2
    exit 1
  fi
}

require_file "$TMP/module.prop"
require_file "$TMP/post-fs-data.sh"
require_file "$TMP/service.sh"
require_file "$TMP/libc_malloc_debug.so"
require_file "$TMP/memory_delegation_broker"

rm -rf "$MOD"
mkdir -p "$MOD/system/lib64" "$MOD/system/bin"

cp "$TMP/module.prop" "$MOD/module.prop"
cp "$TMP/post-fs-data.sh" "$MOD/post-fs-data.sh"
cp "$TMP/service.sh" "$MOD/service.sh"
cp "$TMP/libc_malloc_debug.so" "$MOD/system/lib64/libc_malloc_debug.so"
cp "$TMP/memory_delegation_broker" "$MOD/system/bin/memory_delegation_broker"

chmod 644 "$MOD/module.prop"
chmod 755 "$MOD/post-fs-data.sh" "$MOD/service.sh" \
          "$MOD/system/lib64/libc_malloc_debug.so" \
          "$MOD/system/bin/memory_delegation_broker"
chcon u:object_r:system_file:s0 "$MOD/system/lib64/libc_malloc_debug.so" \
  "$MOD/system/bin/memory_delegation_broker" \
  "$MOD/post-fs-data.sh" "$MOD/service.sh" 2>/dev/null || true

# Safety default: KernelSU treats this marker as disabled.  The user must
# explicitly remove it before rebooting into the system allocator experiment.
echo "disabled by default" > "$MOD/disable"
chmod 644 "$MOD/disable"

# Leave the currently running system clean; this installer does not live-mount.
resetprop -d libc.debug.malloc.options 2>/dev/null || true
resetprop -d libc.debug.malloc.program 2>/dev/null || true

echo "installed disabled module: $MOD"
echo "disable=$(ls "$MOD/disable" 2>/dev/null)"
find "$MOD" -maxdepth 3 -type f | sort
