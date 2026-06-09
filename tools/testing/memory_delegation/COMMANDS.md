# Memory Delegation 常用命令

本文档是 memory delegation 的统一命令入口，合并原
`DEBUG_COMMANDS.md` 和 `MICROBENCH_COMMANDS.md` 中仍然常用的内容。

约定：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
JUMP_HOST=lrc@192.168.61.230
ADB_BIN=/opt/homebrew/bin/adb
ADB_SERIAL=3B15AL00K5D00000
PHONE_DIR=/data/local/tmp/md
```

注意：`tools/testing/memory_delegation/tests/out` 是 Android 和 Linux/QEMU
共用输出目录。切换 `MODE=android` / `MODE=linux` 时必须带 `clean`，否则可能把
Linux 静态 ELF 推到 Android 手机上，表现为 `SharedArena: ready=0` 或执行异常。

## 1. 环境搭建与手机链路

构建并刷入内核：

```bash
cd /home/lrc/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
~/patent/flush_kernel.sh --mode flash
```

检查跳板机 adb：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$JUMP_HOST" \
  "$ADB_BIN devices -l && $ADB_BIN -s $ADB_SERIAL shell getprop sys.boot_completed"
```

构建 Android 测试二进制：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
make -C tools/testing/memory_delegation/tests MODE=android clean \
  scudo_shared_arena_test scudo_shared_arena_latency_bench \
  sched_yield_latency_bench memory_delegation_broker
```

手机 correctness smoke：

```bash
BUILD=1 BUILD_MODE=android USE_SU=1 WAIT_SECS=180 \
tools/testing/memory_delegation/scripts/run_phone_memory_delegation_suite.sh
```

只验证 Android broker / attach 链路：

```bash
BUILD=1 BUILD_MODE=android USE_SU=1 WAIT_SECS=180 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MODE=init_only" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

常用成功标记：

```text
android init mode=attach (system broker ready)
PASS(scudo): shared arena single-process malloc/free ok
PASS(scudo): shared arena malloc/free multi-proc revoke ok
PASS(scudo): shared arena retrieve/store ok
```

查看 broker / kernel 日志：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$JUMP_HOST" \
  "$ADB_BIN -s $ADB_SERIAL shell su -c 'cat /data/local/tmp/md/broker.log 2>/dev/null | tail -n 80; dmesg | tail -n 120'"
```

手机重启或 adb 断开后：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$JUMP_HOST" \
  "$ADB_BIN -s $ADB_SERIAL wait-for-device; \
   $ADB_BIN -s $ADB_SERIAL shell getprop sys.boot.reason; \
   $ADB_BIN -s $ADB_SERIAL shell getprop ro.boot.bootreason; \
   $ADB_BIN -s $ADB_SERIAL shell su -c 'ls -la /sys/fs/pstore 2>/dev/null || true'"
```

## 2. QEMU 简单验证

一键 correctness：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

手动构建 Linux/QEMU 静态测试二进制：

```bash
make -C tools/testing/memory_delegation/tests MODE=linux clean all
```

QEMU benchmark smoke：

```bash
rm -rf /tmp/md-qemu-bench
mkdir -p /tmp/md-qemu-bench

AUTO_EXIT=1 \
DEV_SHM_SIZE=90% \
BUSYBOX=tools/testing/memory_delegation/busybox-1.36.1/busybox \
SCUDO_BENCH_BIN=tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
SCUDO_BENCH_ARGS='--allocator combined --threads 4 --iterations 12 --warmup 4 --sizes 4194304,8388608,16777216,33554432,67108864,134217728,268435456 --touch-pages 1' \
SCUDO_BENCH_ENV='' \
tools/testing/memory_delegation/scripts/qemu_make_initramfs.sh \
  /tmp/md-qemu-bench/initramfs.cpio.gz

RECORD_TTY=1 \
SERIAL_LOG=/tmp/md-qemu-bench/qemu-serial.log \
SMP=4 MEM=4096 \
QEMU_BIN=qemu-system-aarch64 \
KERNEL_IMAGE=/home/lrc/patent/kernel/kernel_platform/out/Image \
INITRAMFS=/tmp/md-qemu-bench/initramfs.cpio.gz \
tools/testing/memory_delegation/scripts/qemu_run_aarch64.sh
```

筛查串口日志：

```bash
rg -n "PASS\\(scudo\\)|FAIL\\(scudo\\)|Kernel panic|Oops|BUG:|Unable to handle|memory_delegation|shared_arena" \
  /tmp/md-qemu-bench/qemu-serial.log /tmp/md-qemu-run/qemu-serial.log
```

解析 QEMU benchmark 图表：

```bash
python3 tools/testing/memory_delegation/scripts/parse_qemu_microbench_log.py \
  --log /tmp/md-qemu-bench/qemu-serial.log \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/qemu-bench \
  --threads 4 \
  --iterations 12 \
  --warmup 4 \
  --repetitions 1 \
  --sizes 4194304,8388608,16777216,33554432,67108864,134217728,268435456
```

## 3. Microbench：内核上下文切换时间

该组测试关注调度切换和 memory delegation PTE sync 增量成本：

- `sched_yield_latency_bench`：普通两个进程绑同一 CPU 的调度底噪。
- `empty_yield`：两个进程处于 SharedArena active 状态，但无 pending log。
- `pending_sync_yield`：`malloc/free` 产生 pending log 后 yield，测量 drain + sync。

构建：

```bash
make -C tools/testing/memory_delegation/tests MODE=android clean \
  sched_yield_latency_bench scudo_shared_arena_test memory_delegation_broker
```

手机侧长脚本已经放在：

```text
tools/testing/memory_delegation/scripts/phone/run_yield_latency_phone.sh
```

推送并运行：

```bash
OUT=/tmp/md-yield-latency-phone-$(date +%Y%m%d-%H%M%S).txt

scp -q \
  tools/testing/memory_delegation/tests/out/sched_yield_latency_bench \
  tools/testing/memory_delegation/tests/out/scudo_shared_arena_test \
  tools/testing/memory_delegation/tests/out/memory_delegation_broker \
  tools/testing/memory_delegation/scripts/phone/run_yield_latency_phone.sh \
  "$JUMP_HOST:/tmp/"

ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$JUMP_HOST" \
  "ADB_BIN=$ADB_BIN ADB_SERIAL=$ADB_SERIAL PHONE_DIR=$PHONE_DIR bash -lc '
   adb_cmd=(\"\$ADB_BIN\"); [[ -n \"\$ADB_SERIAL\" ]] && adb_cmd+=( -s \"\$ADB_SERIAL\" )
   \"\${adb_cmd[@]}\" shell su -c \"mkdir -p \$PHONE_DIR\"
   for f in sched_yield_latency_bench scudo_shared_arena_test memory_delegation_broker run_yield_latency_phone.sh; do
     \"\${adb_cmd[@]}\" push /tmp/\$f \$PHONE_DIR/\$f >/dev/null
   done
   \"\${adb_cmd[@]}\" shell su -c \"cd \$PHONE_DIR && chmod 755 sched_yield_latency_bench scudo_shared_arena_test memory_delegation_broker run_yield_latency_phone.sh && CPU=0 sh ./run_yield_latency_phone.sh\"
  '" | tee "$OUT"
echo "saved: $OUT"
```

关键输出：

```text
YIELD_SWITCH ...
YIELD_STATS tag=empty_switch ...
PENDING_YIELD name=alloc_log_to_next ...
PENDING_YIELD name=free_log_to_next ...
pte_sync ...
delta_runs=... delta_fallbacks=... dirty_chunks=... snapshot_loop=...
```

`chunk_pages` 默认是 `256` 页，也就是 1 MiB。除非专门做 chunk-size sweep，
不要手动修改 `/sys/kernel/debug/memory_delegation/chunk_pages`。如要调参，
必须在 `active_arenas 0` 的干净状态下写入；真机建议每个 chunk 配置之间重启。

## 4. Benchmark：分配器分配/释放时延

该组测试由用户态 Scudo runner 驱动，自动完成：

- 推送 `scudo_shared_arena_latency_bench` 和 `memory_delegation_broker`。
- traditional/delegated 分路径隔离运行。
- delegated 每个 size/path fresh broker。
- 生成 `raw_results.csv`、`summary.csv`、`summary.md` 和 PNG 图。

构建 Android bench：

```bash
make -C tools/testing/memory_delegation/tests MODE=android clean \
  scudo_shared_arena_latency_bench memory_delegation_broker
```

必要注意：

- 真机批量 benchmark 使用 `--use-su`，否则非 root adb shell 可能收不到 broker fd。
- 必须传 `--bench-extra-args "--allocator combined"`，否则默认 secondary-only 不覆盖 Combined allocator 路径。
- 如果刚跑过 QEMU/Linux 构建，务必重新 `MODE=android clean`。

### 4.1 多核心 / 多线程 pin 核测试

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host "$JUMP_HOST" \
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --broker-binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/memory_delegation_broker \
  --thread-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456,536870912,1073741824 \
  --iterations 20 \
  --warmup 10 \
  --repetitions 1 \
  --result-prefix multicore-thread-latency \
  --bench-extra-args "--allocator combined"
```

普通 `thread` 模式中，worker 线程按 benchmark 的 CPU 选择策略分散到可用 arena CPU，
用于观察多核心并行分配释放时延。
如要开启进程模式，在`--bench-extra-args "--allocator combined" `中加入`--mode process`

### 4.2 单核心多线程 / 多进程竞争测试

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host "$JUMP_HOST" \
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --broker-binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/memory_delegation_broker \
  --same-core-contention \
  --contention-modes thread,independent-process \
  --contention-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456,536870912,1073741824 \
  --iterations 20 \
  --warmup 10 \
  --repetitions 1 \
  --result-prefix same-core-contention \
  --bench-extra-args "--allocator combined"
```

输出目录会包含：

```text
raw_results.csv
summary.csv
summary.md
thread_*_latency_*.png
thread_*_speedup_*.png
independent-process_*_latency_*.png
independent-process_*_speedup_*.png
```

### 4.3 快速 sanity

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host "$JUMP_HOST" \
  --use-su \
  --skip-build \
  --binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench \
  --broker-binary /home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/memory_delegation_broker \
  --thread-counts 1 \
  --sizes 16384,131072 \
  --iterations 5 \
  --warmup 1 \
  --repetitions 1 \
  --bench-extra-args "--allocator combined"
```

有效 raw 结果中，traditional 应显示 `SharedArena: ready=0`，delegated 应显示
`SharedArena: ready=1`。

## 5. 日志与源码定位

手机 runner 默认日志：

```text
/tmp/md-phone-run/phone-YYYYMMDD-HHMMSS.out.txt
/tmp/md-phone-run/phone-YYYYMMDD-HHMMSS.dmesg.txt
/tmp/md-phone-run/phone-YYYYMMDD-HHMMSS.logcat.txt
```

快速筛查：

```bash
rg -n "FAIL\\(scudo\\)|PASS\\(scudo\\)|Kernel panic|Oops|BUG:|Unable to handle|SIGSEGV|memory_delegation|shared_arena" \
  /tmp/md-phone-run/phone-*.out.txt \
  /tmp/md-phone-run/phone-*.dmesg.txt \
  /tmp/md-phone-run/phone-*.logcat.txt
```

核心源码入口：

```text
mm/memory_delegation.c
include/linux/memory_delegation.h
include/uapi/linux/memory_delegation.h
kernel/sched/core.c
kernel/fork.c
kernel/sys.c
mm/memory.c
```

快速查找：

```bash
rg -n "memory_delegation_(arena_register|register_ring|on_context_switch|sync_mm|fault_allowed|mm_release|fork_mm)" \
  mm kernel include
```
