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

### 2.0 Android broker / bench 前置条件

Android SharedArena 依赖常驻 `memory_delegation_broker` 分发 arena backing fd。
跑 bench 前先用 correctness runner 构建 Android 测试二进制、清理旧 broker、
启动新 broker，并做一次 `init_only` attach smoke：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=1 BUILD_MODE=android USE_SU=1 WAIT_SECS=180 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MODE=init_only" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

成功标记：

```text
android init mode=attach (system broker ready)
PASS(scudo): shared arena single-process malloc/free ok
```

### 2.0.1 Chunk size 默认配置

内核侧 PTE sync 的 chunk 粒度默认使用 `MD_CHUNK_PAGES_DEFAULT=256`，
对应 256 页，即 1 MiB。该值是基于真机 correctness workload
（4 个子进程轮流持有 32 MiB SharedArena 块，并验证其他进程访问会被
revoke/SIGSEGV）的 32/64/128/256/512 页参数对比后选出的当前推荐值。

除非本轮实验目标就是专门做 chunk-size 调参测试，否则不要修改
`/sys/kernel/debug/memory_delegation/chunk_pages`，也不需要在跑
correctness suite 或 microbenchmark 前手动写这个 debugfs 节点。正常流程下，
保持默认 256 页即可。

如果确实要做 chunk-size sweep，必须在没有已注册 arena 的干净状态下写入
`chunk_pages`。Android broker 一旦注册 arena，内核侧 `active_arenas`
会保持非零；此时直接杀 broker/test 后再写参数会失败或不生效。真机上建议每个
chunk 配置之间重启手机，确认 `chunk_pages` 输出中的 `active_arenas 0`
后再写入新值并启动本轮测试。

当前 microbench 需要注意三点：

- Android 端通过 KernelSU 跑 root 命令，批量 bench 要使用
  `run_shared_arena_microbench.py --use-su`，否则非 root adb shell 可能收不到
  broker 通过 `SCM_RIGHTS` 发送的 memfd，表现为
  `request broker fd core=0 failed status=0 recv=8`。
- benchmark 必须透传 `--bench-extra-args "--allocator combined"`，默认
  `secondary_only` 不会触发 Combined allocator / SharedArena 路径。
- benchmark 二进制要包含
  `-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL`。内核测试 Makefile 的
  `MODE=android scudo_shared_arena_latency_bench` 目标已带该宏；Scudo standalone
  的手工 NDK fallback 也应带同样宏。

构建 Android bench：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
make -C tools/testing/memory_delegation/tests MODE=android \
  scudo_shared_arena_latency_bench memory_delegation_broker
```

输出：

```text
/home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench
/home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/memory_delegation_broker
```

sanity：确认 root + combined 后 delegated 路径可用：

```bash
RESULTS_DIR=/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-sanity-su-$(date +%Y%m%d-%H%M)
mkdir -p "${RESULTS_DIR}"

python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.4 \
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --thread-counts 1 \
  --sizes 1048576,33554432 \
  --iterations 10 \
  --warmup 2 \
  --repetitions 1 \
  --bench-extra-args "--allocator combined" \
  --results-dir "${RESULTS_DIR}" \
  2>&1 | tee "${RESULTS_DIR}/run.log"
```

有效输出开头应包含：

```text
SharedArena: ready=1 num_cores=8 page_size=4096
-- delegated (SharedArena) --
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
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --thread-counts 1,2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 \
  --warmup 20 \
  --repetitions 3 \
  --bench-extra-args "--allocator combined" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-pow2-kib-4-18-1t2t-su-$(date +%Y%m%d-%H%M)
```

### 2.3 2026-05-13 真机多轮稳定性记录

本轮目的：手机上跑 microbench 多轮，观察大块 delegated 路径是否还会导致
ADB 断连/设备重启。

前置 broker 启动命令：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=1 BUILD_MODE=android USE_SU=1 WAIT_SECS=180 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MODE=init_only" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

有效 sanity 结果：

```text
results: /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-sanity-su-20260513-1810
raw:     raw/threads01_rep01.txt
marker:  SharedArena: ready=1 num_cores=8 page_size=4096
marker:  -- delegated (SharedArena) --
```

正式有效多轮命令：

```bash
RESULTS_DIR=/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-pow2-kib-4-18-1t2t-su-3rep-20260513-1811
mkdir -p "${RESULTS_DIR}"

python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.4 \
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --thread-counts 1,2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 \
  --warmup 20 \
  --repetitions 3 \
  --bench-extra-args "--allocator combined" \
  --results-dir "${RESULTS_DIR}" \
  2>&1 | tee "${RESULTS_DIR}/run.log"
```

观察结果：

- `threads=1, repetition=1` 完整跑完，`raw/threads01_rep01.txt` 中
  `SharedArena: ready=1`，解析到 120 条记录。
- 进入 `threads=2, repetition=1` 时 adb 设备消失，脚本收到 ssh/adb
  exit 255。
- 手机约 86 秒后重新上线，随后 `sys.boot_completed=1`。
- `sys.boot.reason=reboot`，`ro.boot.bootreason=reboot`。
- `/sys/fs/pstore` 为空；未抓到持久化 kernel panic/oops。
- 重启后的 dmesg 有 vendor tracepoint WARN：
  `tracepoint_add_func+0x228/0x438`，进程为 `autochmod.sh`，未直接指向
  `memory_delegation`。
- 留档：
  - `/tmp/md-phone-microbench-reboot-20260513-1811.logcat.txt`
  - `/tmp/md-phone-microbench-reboot-20260513-1811.dmesg.txt`
  - `/tmp/md-phone-microbench-reboot-20260513-1811.pstore.txt`

无效尝试记录：

- `phone-kernel-scudo-pow2-kib-4-18-1t2t-5rep-20260513-1750`：
  使用 Scudo standalone 脚本默认构建的 bench，全部输出
  `SharedArena: ready=0` / `delegated skipped (pool not ready)`，只能说明传统
  路径和设备在这轮下未立刻挂，不能作为 delegated 性能数据。
- `phone-kernel-scudo-pow2-kib-4-18-1t2t-valid-3rep-20260513-1758`：
  使用内核 Makefile bench 但未加 `--use-su`，非 root adb shell 无法有效接收
  broker fd，同样 `SharedArena: ready=0`。
- `phone-kernel-scudo-sanity-combined-20260513-1802` /
  `phone-kernel-scudo-sanity-fixed-20260513-1805`：确认仅加
  `--allocator combined` 或仅修 benchmark init 仍不够；需要 root 运行。


如需确认系统 broker 状态：

```bash
ssh lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell \"su -c 'ps -A | grep -E \\\"memory_delegation_broker|scudo_shared|ScudoShared\\\" || true; \
                                 cat /data/local/tmp/md/broker.log 2>/dev/null | tail -n 40 || true'\""
```

当前 Android 方案使用常驻 `memory_delegation_broker` 持有 arena backing；
应用进程不会创建或 fork broker。测试脚本默认会在运行前清理旧 broker 并启动新 broker。

手动跑真机 bench 前需要保证 broker 已经在手机侧运行。可以复用 correctness
runner 的 broker 管理逻辑先启动一次：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=1 BUILD_MODE=android USE_SU=1 START_BROKER=1 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_MODE=init_only SCUDO_SHARED_ARENA_TEST_CPU=0" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

## 3. QEMU benchmark

当前 QEMU 已用于验证“新内核 + 改造 Scudo”在大块分配上的修复效果。
最新稳定验证配置是 4 线程、`4 MiB` 到 `256 MiB`、`--touch-pages 1`，
完整跑完并正常 poweroff。

QEMU 使用 `MODE=linux` 静态 ELF，SharedArena backing 走 Linux
`shm_open`/`/dev/shm` 路径，不依赖 Android 的 `memory_delegation_broker`。

### 3.0 QEMU correctness smoke

一键正确性测试：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

最近一次通过记录：

```text
2026-05-13
log: /tmp/md-qemu-run/qemu-serial.log
result: PASS: scudo_shared_arena_test succeeded under QEMU (3 runs)
```

覆盖项包括 pressure toggle、single-process、multi-thread、lifecycle、
CPU 0/1 retrieve-store，以及 CPU 0 -> CPU 1 cross-free migration。

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
