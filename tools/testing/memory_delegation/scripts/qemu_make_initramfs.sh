#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Create a tiny initramfs for QEMU debugging.
#
# Output is a gzip-compressed newc cpio archive containing:
#   - /init (mounts proc/sys/dev and runs tests)
#   - /bin/busybox (+ symlinks)
#   - optional /bin/scudo_shared_arena_test
#
set -euo pipefail

OUT="${1:-}"
[[ -n "${OUT}" ]] || { echo "Usage: $0 /path/to/initramfs.cpio.gz" >&2; exit 2; }

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need cpio
need gzip
need file

BUSYBOX="${BUSYBOX:-}"
[[ -n "${BUSYBOX}" ]] || { echo "missing BUSYBOX=/path/to/aarch64-busybox" >&2; exit 1; }

desc="$(file -b "${BUSYBOX}" || true)"
echo "${desc}" | grep -qiE 'aarch64|ARM aarch64' || { echo "BUSYBOX is not aarch64: ${desc}" >&2; exit 1; }

AUTO_EXIT="${AUTO_EXIT:-0}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}"/{bin,sbin,proc,sys,dev,dev/shm,tmp,run,etc,root}
install -m 0755 "${BUSYBOX}" "${WORK}/bin/busybox"

SCUDO_TEST_BIN="${SCUDO_TEST_BIN:-}"
if [[ -n "${SCUDO_TEST_BIN}" ]]; then
  install -m 0755 "${SCUDO_TEST_BIN}" "${WORK}/bin/scudo_shared_arena_test"
fi

cat >"${WORK}/init" <<EOF
#!/bin/busybox sh
set -eux
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev || true
mount -t tmpfs tmpfs /dev/shm || true
echo
echo "initramfs: booted. uname:"
uname -a || true
echo
if [ -x /bin/scudo_shared_arena_test ]; then
  echo "Running scudo_shared_arena_test ..."
  # Must be set before allocator initialization (can happen before main()).
  export SCUDO_SHARED_ARENA_FORCE=1
  export SCUDO_SHARED_ARENA_TRACE=1
  /bin/scudo_shared_arena_test || echo "scudo_shared_arena_test failed (see output above)"
  echo
fi
if [ "${AUTO_EXIT}" = "1" ]; then
  echo "AUTO_EXIT=1: powering off"
  /bin/busybox poweroff -f || /bin/busybox halt -f || exit 0
fi
echo "Dropping to shell. Use 'dmesg' to inspect logs."
exec /bin/busybox sh
EOF
chmod +x "${WORK}/init"

(
  cd "${WORK}"
  for app in sh mount umount ls cat dmesg echo grep ps top uname sleep hexdump poweroff halt; do
    ln -sf busybox "bin/${app}" || true
  done
  find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${OUT}"
)

echo "Wrote ${OUT}"

