#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check IRQ state inside the ctx-table critical section, then stress exit.

Use a fresh QEMU guest; probe saved ARM64 PSTATE, not trace's IRQ flag (the
probe handler itself masks interrupts even when the interrupted code did not).
An unfixed kernel fails IRQ_STATE even if its probabilistic stress run passes.
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
for name in ("image", "vmlinux", "busybox", "test-binary", "out-dir"):
    p.add_argument("--" + name, required=True, type=pathlib.Path)
p.add_argument("--objdump", default="llvm-objdump")
p.add_argument("--qemu", default="qemu-system-aarch64")
p.add_argument("--loops", type=int, default=2000)
p.add_argument("--arena-mb", type=int, default=2048)
a = p.parse_args()
if a.loops < 1 or not 1 <= a.arena_mb <= 2048:
    p.error("loops must be positive; arena-mb must be in [1, 2048]")
function = "memory_delegation_mm_release"
asm = subprocess.check_output([a.objdump, "-d", "--no-show-raw-insn",
                              "--disassemble-symbols=" + function,
                              str(a.vmlinux)], text=True)
base = int(re.search(r"([0-9a-f]+) <" + function + r">:", asm)[1], 16)
call = re.search(r"^([0-9a-f]+):\s+bl\s+[^\n]*<_raw_spin_lock(?:_irqsave)?>", asm, re.M)
if not call:
    sys.exit("Cannot locate ctx lock acquisition; audit new disassembly first")
offset = int(call[1], 16) + 4 - base
out = a.out_dir.resolve()
root = out / "root"
(root / "bin").mkdir(parents=True, exist_ok=True)
for name in ("proc", "sys", "dev", "tmp"):
    (root / name).mkdir(exist_ok=True)
for source, target in ((a.busybox, "busybox"), (a.test_binary, "ctx_irq_exit_stress")):
    shutil.copyfile(source, root / "bin" / target)
    (root / "bin" / target).chmod(0o755)
if not (root / "bin/sh").exists():
    (root / "bin/sh").symlink_to("busybox")
init = f'''#!/bin/sh
set -eu
export PATH=/bin
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mount -t tracefs tracefs /sys/kernel/tracing
busybox mknod -m 666 /dev/null c 1 3
ulimit -c 0
echo 'p:ctx_held {function}+{offset:#x} pstate=%pstate:x64' > /sys/kernel/tracing/kprobe_events
echo 1 > /sys/kernel/tracing/events/kprobes/ctx_held/enable
/bin/ctx_irq_exit_stress {a.loops} {a.arena_mb}
echo 0 > /sys/kernel/tracing/events/kprobes/ctx_held/enable
echo IRQ_TRACE_BEGIN
busybox cat /sys/kernel/tracing/trace
echo IRQ_TRACE_END
echo CASE_EXIT=0
busybox poweroff -f
'''
(root / "init").write_text(init)
(root / "init").chmod(0o755)
with (out / "initramfs.gz").open("wb") as f:
    subprocess.run("find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -1",
                   cwd=root, shell=True, stdout=f, check=True)
cmd = ["timeout", "150", a.qemu, "-machine", "virt,gic-version=3", "-cpu", "cortex-a57",
       "-smp", "4", "-m", "1024", "-nographic", "-no-reboot", "-kernel", str(a.image.resolve()),
       "-initrd", str(out / "initramfs.gz"), "-append",
       "console=ttyAMA0 rdinit=/init panic=1 oops=panic loglevel=4"]
with (out / "serial.log").open("w") as f:
    result = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
log = (out / "serial.log").read_text(errors="replace")
states = [int(x, 16) for x in re.findall(r"ctx_held:.*?pstate=0x([0-9a-f]+)", log)]
irq_ok = bool(states) and all(x & 0x80 for x in states)
ok = (result.returncode == 0 and "CASE_EXIT=0" in log and irq_ok and
      not any(x in log for x in ("Kernel panic", "BUG:", "WARNING:", "Oops:")))
print(f"IRQ_STATE {'PASS' if irq_ok else 'FAIL'} samples={len(states)} "
      f"irq_masked={sum(bool(x & 0x80) for x in states)} probe_offset={offset:#x}")
for line in log.splitlines():
    if line.startswith("PASS(ctx_irq_exit)"):
        print(line)
print(f"CTX_IRQ_REGRESSION {'PASS' if ok else 'FAIL'} log={out / 'serial.log'}")
sys.exit(0 if ok else 1)
