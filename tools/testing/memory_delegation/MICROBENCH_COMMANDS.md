# Scudo SharedArena Microbenchmark Commands

本文档记录 Scudo SharedArena 在真机和 QEMU 中运行正确性测试、microbenchmark
并生成图表时用到的主要命令。

## 1. 真机正确性 smoke suite

当前手机已经刷入 memory delegation 新内核，真机侧只需要编译并部署用户态
Scudo 测试程序。

在内核 common 目录执行：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=1 BUILD_MODE=android USE_SU=1 WAIT_SECS=180 \
  tools/testing/memory_delegation/scripts/run_phone_memory_delegation_suite.sh
```

最近一次完整通过日志：

```text
/tmp/md-phone-run/phone-20260507-201745.out.txt
/tmp/md-phone-run/phone-20260507-201751.out.txt
/tmp/md-phone-run/phone-20260507-201755.out.txt
/tmp/md-phone-run/phone-20260507-201800.out.txt
```

关键通过标记：

```text
PASS(scudo): shared arena single-process malloc/free ok
PASS(scudo): shared arena malloc/free multi-proc revoke ok
PASS(scudo): shared arena retrieve/store ok
PASS: phone memory delegation suite completed
```

## 2. 真机 microbenchmark

真机 microbenchmark 脚本位于 Scudo standalone 目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py
```

### 2.1 已跑通的 1/2 线程小范围测试

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.4 \
  --thread-counts 1,2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152 \
  --iterations 80 \
  --warmup 20 \
  --repetitions 1 \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-20260507-partial-1t2t
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-20260507-partial-1t2t
```

### 2.2 1/2 线程、16 KiB 到 256 MiB 测试命令

本轮要测试的是 `2^4 KiB` 到 `2^18 KiB`，也就是 `16 KiB` 到
`256 MiB`。

`shared_arena_latency_bench` 的 `--sizes` 参数单位是 bytes，因此需要把 KiB
转成 bytes：

```text
16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456
```

对应关系：

```text
2^4  KiB = 16 KiB  = 16384 bytes
2^5  KiB = 32 KiB  = 32768 bytes
2^6  KiB = 64 KiB  = 65536 bytes
2^7  KiB = 128 KiB = 131072 bytes
2^8  KiB = 256 KiB = 262144 bytes
2^9  KiB = 512 KiB = 524288 bytes
2^10 KiB = 1 MiB   = 1048576 bytes
...
2^18 KiB = 256 MiB = 268435456 bytes
```

建议先只跑 1/2 线程；此前 4/8 线程真机长跑时出现过 adb/设备连接不稳定。
当前更适合把 4 线程以上的大块压力先放在 QEMU 中验证，真机侧用于
1/2 线程稳定数据采集；如果要跑 4/8 线程真机长跑，建议单独开一次实验，
并提前准备 adb/设备断连后的续跑或重跑策略。

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.4 \
  --thread-counts 1,2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 \
  --warmup 20 \
  --repetitions 3 \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-pow2-kib-4-18-1t2t-20260507
```


如需确认 broker 没有残留：

```bash
ssh lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell 'ps -A | grep -E \"scudo_shared|ScudoShared\" || true'"
```

当前 broker 伴随进程方案中，父进程退出后 broker 会收到 `SIGTERM` 自动退出；
正常情况下应用和测试脚本不需要手工清理 broker。

## 3. QEMU benchmark

当前 QEMU 已用于验证“新内核 + 改造 Scudo”在大块分配上的修复效果。
最新稳定验证配置是 4 线程、`4 MiB` 到 `256 MiB`、`--touch-pages 1`，
完整跑完并正常 poweroff。

### 3.1 编译 benchmark

在内核 common 目录执行：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

make -C tools/testing/memory_delegation/tests MODE=linux clean scudo_shared_arena_latency_bench
```

输出二进制：

```text
tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench
```

该目标会把 Scudo standalone 相关源码和
`/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/tests/shared_arena_latency_bench.cpp`
一起编成 aarch64 静态 ELF，用于放进 QEMU initramfs。

注意：QEMU initramfs 中应使用 `MODE=linux` 生成的静态 Linux ELF。
不要把 Android 动态 PIE 直接放入 initramfs，否则容易出现
`env: can't execute '/bin/scudo_shared_arena_latency_bench': No such file or directory`。

### 3.2 生成 QEMU initramfs

#### 3.2.1 修复后大块稳定验证配置（推荐）

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

rm -rf /tmp/md-qemu-bench-fix-rerun2
mkdir -p /tmp/md-qemu-bench-fix-rerun2

AUTO_EXIT=1 \
DEV_SHM_SIZE=90% \
BUSYBOX=tools/testing/memory_delegation/busybox-1.36.1/busybox \
SCUDO_BENCH_BIN=tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
SCUDO_BENCH_ARGS='--allocator combined --threads 4 --iterations 12 --warmup 4 --sizes 4194304,8388608,16777216,33554432,67108864,134217728,268435456 --touch-pages 1' \
SCUDO_BENCH_ENV='' \
tools/testing/memory_delegation/scripts/qemu_make_initramfs.sh \
  /tmp/md-qemu-bench-fix-rerun2/initramfs.cpio.gz
```

说明：

- `SCUDO_BENCH_ENV=''` 表示不通过进程环境全局设置 `SCUDO_SHARED_ARENA_FORCE=1`。
- benchmark 内部通过 `setSharedArenaForceForTesting(true/false)` 区分 delegated 和 traditional 路径。
- delegated 路径强制开启 arena；traditional 路径强制关闭 arena。
- `DEV_SHM_SIZE=90%` 很重要。4 个 512 MiB arena 在 QEMU 默认 tmpfs
  半内存大小下可能触发 `SIGBUS`，这会干扰内核 bug 判断。
- 这里的 `iterations=12/warmup=4` 是修复验证配置，目的是快速确认大块路径
  不再崩溃；需要正式性能数据时再提高到 `iterations=80/warmup=20`。

### 3.3 启动 QEMU 并运行 benchmark

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

RECORD_TTY=1 \
SERIAL_LOG=/tmp/md-qemu-bench-fix-rerun2/qemu-serial.log \
SMP=4 \
MEM=4096 \
QEMU_BIN=qemu-system-aarch64 \
KERNEL_IMAGE=/home/lrc/patent/kernel/kernel_platform/out/Image \
INITRAMFS=/tmp/md-qemu-bench-fix-rerun2/initramfs.cpio.gz \
tools/testing/memory_delegation/scripts/qemu_run_aarch64.sh
```

串口日志：

```text
/tmp/md-qemu-bench-fix-rerun2/qemu-serial.log
```

最新稳定结果：

- 4 线程、`--touch-pages 1`、`MEM=4096`：`4 MiB` 到 `256 MiB`
  全部完整输出。
- 串口日志未发现 `Kernel panic`、`Oops`、`stack-protector`、
  `memory_delegation: drop log`、`Scudo ERROR/CHECK`。
- benchmark 结束后 `AUTO_EXIT=1` 正常 poweroff。

修复前历史现象：

- 4 线程、`--touch-pages 1`：曾完整输出到 `64 MiB`，进入 `128 MiB`
  时触发 `Bus error`。后续确认和 QEMU `/dev/shm` 容量有关，不能直接
  归因于内核 delegation。
- 1 线程、`--touch-pages 1`、`MEM=4096`：曾在 `128 MiB` 后触发
  `memory_delegation_on_context_switch` stack protector panic。
- 1 线程、`--touch-pages 0`、`MEM=4096`：曾在 `256 MiB` 前触发同类
  stack protector panic。
- 根因之一是内核 `MD_SHARED_ARENA_CAPACITY=256MiB` 与 Scudo
  `kArenaCapacityPerCore=512MiB` 不一致，导致 drain log 的固定栈位图越界。
  当前内核已改为 512 MiB，并增加 register 边界检查。

### 3.4 从串口日志解析并画图

复用用户态 Scudo 目录中的绘图脚本函数：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py
```

本轮已整理成独立脚本：

```text
/home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/scripts/parse_qemu_microbench_log.py
```

使用下面的命令解析 QEMU 串口日志，并生成 CSV、summary 和 PNG：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

python3 tools/testing/memory_delegation/scripts/parse_qemu_microbench_log.py \
  --log /tmp/md-qemu-bench-fix-rerun2/qemu-serial.log \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507-parser \
  --threads 4 \
  --iterations 12 \
  --warmup 4 \
  --repetitions 1 \
  --sizes 4194304,8388608,16777216,33554432,67108864,134217728,268435456
```

已生成的修复验证图表目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507
```

主要文件：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507/summary.csv
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507/summary.md
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507/latency_comparison_4t_large_sizes.png
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507/speedup_lines_4t_large_sizes.png
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-fix-rerun2-20260507/end_to_end_summary_4t_large_sizes.png
```

`summary.md` 中的端到端结果：

```text
4 MiB   delegated=0.561 ms   traditional=63.729 ms    speedup=113.59x
8 MiB   delegated=0.764 ms   traditional=124.400 ms   speedup=162.92x
16 MiB  delegated=1.283 ms   traditional=253.650 ms   speedup=197.76x
32 MiB  delegated=2.048 ms   traditional=510.358 ms   speedup=249.22x
64 MiB  delegated=4.097 ms   traditional=1013.266 ms  speedup=247.35x
128 MiB delegated=7.777 ms   traditional=2018.457 ms  speedup=259.54x
256 MiB delegated=15.502 ms  traditional=4044.883 ms  speedup=260.93x
```

历史部分结果仍保留在：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-pow2-kib-4-18-1t-notouch-20260507
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-pow2-kib-4-18-1t-touch-20260507-partial
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-pow2-kib-4-18-4t-touch-20260507-partial
```
