#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Minimal QEMU runner for debugging common arm64 GKI kernel changes.
#
set -euo pipefail

QEMU_BIN="${QEMU_BIN:-qemu-system-aarch64}"
KERNEL_IMAGE="${KERNEL_IMAGE:-}"
INITRAMFS="${INITRAMFS:-}"

MEM="${MEM:-2048}"          # MiB
SMP="${SMP:-4}"
CPU="${CPU:-cortex-a57}"

SERIAL_LOG="${SERIAL_LOG:-qemu-serial.log}"
RECORD_TTY="${RECORD_TTY:-0}" # 1 => record full interactive session via `script`
APPEND_EXTRA="${APPEND_EXTRA:-}"

die() { echo "ERROR: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"; }

need "${QEMU_BIN}"

[[ -n "${KERNEL_IMAGE}" ]] || die "set KERNEL_IMAGE=/path/to/Image"
[[ -f "${KERNEL_IMAGE}" ]] || die "KERNEL_IMAGE not found: ${KERNEL_IMAGE}"

QEMU_ARGS=(
  -machine virt,gic-version=3,virtualization=on
  -cpu "${CPU}"
  -smp "${SMP}"
  -m "${MEM}"

  -nographic
  -chardev stdio,mux=on,id=char0,signal=off
  -serial chardev:char0
  -mon chardev=char0,mode=readline

  -no-reboot
  -d guest_errors

  -kernel "${KERNEL_IMAGE}"
  -device virtio-rng-pci
)

KCMDLINE=(
  "console=ttyAMA0,115200"
  "earlycon=pl011,0x09000000"
  "loglevel=8"
  "ignore_loglevel"
  "panic=1"
  "oops=panic"
  "printk.devkmsg=on"
  "initcall_debug"
)

if [[ -n "${INITRAMFS}" ]]; then
  [[ -f "${INITRAMFS}" ]] || die "INITRAMFS not found: ${INITRAMFS}"
  QEMU_ARGS+=(-initrd "${INITRAMFS}")
  KCMDLINE+=("rdinit=/init")
fi

if [[ -n "${APPEND_EXTRA}" ]]; then
  # shellcheck disable=SC2206
  KCMDLINE+=(${APPEND_EXTRA})
fi

echo "KERNEL_IMAGE=${KERNEL_IMAGE}"
echo "INITRAMFS=${INITRAMFS:-<none>}"
echo "SERIAL_LOG=${SERIAL_LOG} (set RECORD_TTY=1 to record full session)"
echo "CMDLINE: ${KCMDLINE[*]}"
echo
echo "Switch monitor/serial: Ctrl-a then c"
echo

if [[ "${RECORD_TTY}" == "1" ]]; then
  command -v script >/dev/null 2>&1 || die "RECORD_TTY=1 requires util-linux 'script'"
  exec script -q -f -c \
    "$(printf '%q ' "${QEMU_BIN}" "${QEMU_ARGS[@]}" -append "${KCMDLINE[*]}")" \
    "${SERIAL_LOG}"
else
  exec "${QEMU_BIN}" "${QEMU_ARGS[@]}" -append "${KCMDLINE[*]}"
fi

