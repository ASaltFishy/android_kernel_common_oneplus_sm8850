## Memory delegation QEMU tests (arm64)

This is a *host-side* workflow to debug early panics and logic issues in the common arm64 kernel.
It does **not** emulate your phone's SoC / vendor drivers.

### 1) Build the kernel Image (example)

From `kernel_platform/`:

```sh
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
```

You should have `out/Image`.

### One-shot (recommended)

```sh
common/tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

This builds test binaries with `tests/Makefile`, packs initramfs, boots QEMU, and
checks PASS markers in the serial log.

### Manual steps (debug)

```sh
make -C common/tools/testing/memory_delegation/tests clean all

AUTO_EXIT=0 \
BUSYBOX=common/tools/testing/memory_delegation/busybox-1.36.1/busybox \
SCUDO_TEST_BIN=common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_test \
common/tools/testing/memory_delegation/scripts/qemu_make_initramfs.sh /tmp/initramfs.cpio.gz

RECORD_TTY=1 SERIAL_LOG=qemu-serial.log \
KERNEL_IMAGE=out/Image INITRAMFS=/tmp/initramfs.cpio.gz \
common/tools/testing/memory_delegation/scripts/qemu_run_aarch64.sh
```

Kernel logs are written to `qemu-serial.log` by default.

### Useful knobs

- Increase verbosity: `APPEND_EXTRA="debug"` or `APPEND_EXTRA="dyndbg=+p"`
- Change CPU count / memory:
  - `SMP=8 MEM=4096`
- If it panics:
  - inspect `qemu-serial.log` for the call trace

