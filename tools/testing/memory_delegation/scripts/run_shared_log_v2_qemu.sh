#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail
BASE=$(cd "$(dirname "$0")/.." && pwd)
WORKDIR=${WORKDIR:-$(mktemp -d /tmp/md-shared-v2.XXXXXX)}
KERNEL_IMAGE=${KERNEL_IMAGE:?set KERNEL_IMAGE to the newly built v3 Image}
BUSYBOX=${BUSYBOX:-$BASE/busybox-1.36.1/busybox}
CC=${CC:-aarch64-linux-gnu-gcc}
mkdir -p "$WORKDIR/root/bin" "$WORKDIR/root/proc" "$WORKDIR/root/sys" "$WORKDIR/root/dev"
"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -static -pthread \
  "$BASE/tests/shared_log_v2_test.c" -o "$WORKDIR/root/mdtest"
cp "$BUSYBOX" "$WORKDIR/root/bin/busybox"
ln -sf busybox "$WORKDIR/root/bin/sh"
cat > "$WORKDIR/root/init" <<'INIT'
#!/bin/sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
ulimit -c 0
/mdtest
rc=$?
echo "MD_V2_TEST_EXIT=$rc"
/bin/busybox poweroff -f
INIT
chmod +x "$WORKDIR/root/init"
(cd "$WORKDIR/root" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1) > "$WORKDIR/initramfs.gz"
timeout "${TIMEOUT:-120}" "${QEMU_BIN:-qemu-system-aarch64}" \
  -machine virt,gic-version=3 -cpu cortex-a57 -smp "${SMP:-4}" -m 1024 \
  -nographic -no-reboot -kernel "$KERNEL_IMAGE" -initrd "$WORKDIR/initramfs.gz" \
  -append 'console=ttyAMA0 rdinit=/init panic=1 oops=panic loglevel=6' \
  > "$WORKDIR/serial.log" 2>&1
cat "$WORKDIR/serial.log"
if grep -Eq 'Kernel panic|BUG:|Oops:|FAIL\(v3\):' "$WORKDIR/serial.log"; then
  exit 1
fi
grep -q 'MD_V2_TEST_EXIT=0' "$WORKDIR/serial.log"
grep -q 'PASS(v3): ALL shared Arena protocol tests' "$WORKDIR/serial.log"
echo "PASS: shared log v3 QEMU suite; log=$WORKDIR/serial.log"
