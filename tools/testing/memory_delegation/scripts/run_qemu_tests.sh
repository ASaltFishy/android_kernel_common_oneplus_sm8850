#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# One-shot runner:
#   - make aarch64 static scudo test binary
#   - pack initramfs
#   - boot QEMU
#   - assert PASS markers in log
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"                    # memory_delegation/
TESTS_DIR="${BASE_DIR}/tests"

QEMU_BIN="${QEMU_BIN:-qemu-system-aarch64}"
KERNEL_PLATFORM_DIR="$(cd "${BASE_DIR}/../../../.." && pwd)"  # kernel_platform/
KERNEL_IMAGE="${KERNEL_IMAGE:-${KERNEL_PLATFORM_DIR}/out/Image}"

BUSYBOX="${BUSYBOX:-${BASE_DIR}/busybox-1.36.1/busybox}"

WORKDIR="${WORKDIR:-/tmp/md-qemu-run}"
mkdir -p "${WORKDIR}"

LOG="${LOG:-${WORKDIR}/qemu-serial.log}"
INITRAMFS="${INITRAMFS:-${WORKDIR}/initramfs.cpio.gz}"

SMP="${SMP:-4}"
MEM="${MEM:-2048}"

die() { echo "ERROR: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"; }

need make
need "${QEMU_BIN}"
need cpio
need gzip
need file
need script
need aarch64-linux-gnu-gcc
need aarch64-linux-gnu-g++

[[ -f "${KERNEL_IMAGE}" ]] || die "KERNEL_IMAGE not found: ${KERNEL_IMAGE}"
[[ -f "${BUSYBOX}" ]] || die "BUSYBOX not found: ${BUSYBOX}"

echo "== Build test binaries (Makefile) =="
make -C "${TESTS_DIR}" clean all

SCUDO_TEST_BIN="${TESTS_DIR}/out/scudo_shared_arena_test"

echo "== Build initramfs =="
rm -f "${INITRAMFS}"
AUTO_EXIT=1 BUSYBOX="${BUSYBOX}" SCUDO_TEST_BIN="${SCUDO_TEST_BIN}" \
  "${SCRIPT_DIR}/qemu_make_initramfs.sh" "${INITRAMFS}"

echo "== Boot QEMU and run tests =="
rm -f "${LOG}"
RECORD_TTY=1 SERIAL_LOG="${LOG}" SMP="${SMP}" MEM="${MEM}" QEMU_BIN="${QEMU_BIN}" \
KERNEL_IMAGE="${KERNEL_IMAGE}" INITRAMFS="${INITRAMFS}" \
  "${SCRIPT_DIR}/qemu_run_aarch64.sh"

echo "== Check results =="
grep -Fq "PASS(scudo): shared arena retrieve/store ok" "${LOG}" || {
  echo "FAIL: scudo_shared_arena_test PASS marker missing" >&2
  tail -n 120 "${LOG}" >&2 || true
  exit 1
}
echo "PASS: scudo_shared_arena_test succeeded under QEMU"

echo "Log: ${LOG}"

