# Memory Delegation 测试与 Debug 命令记录

本文档记录 memory delegation 当前在 QEMU 和手机上验证时用到的命令、脚本入口、环境变量和日志排查手段。

## 1. 内核构建

在 `kernel_platform/` 目录构建当前 common kernel，并输出到 `out/`：

```bash
cd ~/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
```

关键产物：

```bash
ls -lh ~/patent/kernel/kernel_platform/out/Image
```

## 2. 刷入手机内核

当前内核相对稳定时，推荐使用 flash/flush 模式固化刷入，避免测试后设备重启又回到旧基线：

```bash
cd ~/patent/kernel/kernel_platform
~/patent/flush_kernel.sh --mode flash
```

脚本当前链路：

- 本地 `out/Image` 更新到 `/home/lrc/patent/Image`
- 生成/更新 `/home/lrc/patent/boot.img`
- 通过 ssh 复制到 macOS 跳板机
- 在跳板机上使用 fastboot flash/boot 流程刷入该 boot image

注意：若临时调试需要使用 `boot` 模式，可以直接执行 `~/patent/flush_kernel.sh`。但临时 boot 在手机重启后会丢失；当前调试建议优先使用 `--mode flash`。

## 3. 手机测试入口

测试目录：

```bash
cd ~/patent/kernel/kernel_platform/common
```

推荐的分阶段手机 smoke suite：

```bash
tools/testing/memory_delegation/scripts/run_phone_memory_delegation_suite.sh
```

该 suite 会按风险递增顺序运行：

- `init_only`：只验证 Scudo SharedArena 初始化、memfd/mmap、arena 注册等基础链路
- `single-cpu revoke/free`：验证同 CPU 多进程 ownership 提交、撤销映射、非法访问 SIGSEGV
- `cross-cpu free migration`：验证跨 CPU free 后延迟提交路径
- `short stress`：增加轮次做短压力测试

底层手机 runner：

```bash
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

常用变量：

```bash
JUMP_HOST=lrc@192.168.61.4
ADB_BIN=/opt/homebrew/bin/adb
ADB_SERIAL=3B15AL00K5D00000
PHONE_DIR=/data/local/tmp/md
BUILD=1
BUILD_MODE=android
USE_SU=0
WAIT_SECS=120
WAIT_BOOT_COMPLETED=1
```

`WAIT_BOOT_COMPLETED=1` 为默认值，runner 会等待 `sys.boot_completed=1` 后再 push/run，避免系统尚未完全启动时开始测试。

示例：只跑初始化，用于确认手机链路是否可用：

```bash
BUILD=1 BUILD_MODE=android \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MODE=init_only" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

示例：跑单 CPU revoke/free，限制轮次便于快速定位：

```bash
BUILD=0 WAIT_SECS=180 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

示例：跑跨 CPU free：

```bash
BUILD=0 WAIT_SECS=180 \
EXTRA_ENV="SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_FREE_CPU=1 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

runner 默认会给测试程序追加：

```bash
SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=1
```

## 4. 手机侧手动 ADB Debug

通过 macOS 跳板机检查设备：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb devices -l"
```

等待手机重新上线：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb wait-for-device"
```

直接在手机上运行已 push 的测试二进制：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell 'cd /data/local/tmp/md && SCUDO_SHARED_ARENA_FORCE=1 SCUDO_SHARED_ARENA_TRACE=1 SCUDO_SHARED_ARENA_TEST_CPU=0 ./scudo_shared_arena_test'"
```

抓 logcat：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb logcat -d -v time | tail -n 400"
```

如果 shell 没有 dmesg 权限，可尝试 root：

```bash
USE_SU=1 tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

或手动：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell \"su 0 -c 'dmesg | tail -n 400'\""
```

### 4.1 内核上下文切换 cycle 统计

当前内核在 `CONFIG_MEMORY_DELEGATION_DEBUGFS` 下提供 memory delegation
调度路径的 cycle 统计入口。该配置依赖 `CONFIG_DEBUG_FS`：

```text
/sys/kernel/debug/memory_delegation/switch_cycle_stats
/sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled
```

该统计使用 ARM64 `get_cycles()`，也就是 generic timer `cntvct` counter。
它适合在同一台机器上做相对比较；注意它不一定等同于 CPU core PMU
cycle。

确认 debugfs 文件存在：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb -s 3B15AL00K5D00000 shell \
   'su 0 -c \"mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true; \
             head -n 4 /sys/kernel/debug/memory_delegation/switch_cycle_stats\"'"
```

跑 `scudo_shared_arena_test` 并统计包含 PTE sync 的内核增量路径：

```bash
OUT=/tmp/md-switch-pte-cycle-stats-$(date +%Y%m%d-%H%M%S).txt
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb -s 3B15AL00K5D00000 shell \
   'su 0 -c \"echo reset > /sys/kernel/debug/memory_delegation/switch_cycle_stats; \
             echo 1 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled\"; \
    cd /data/local/tmp/md && \
    SCUDO_SHARED_ARENA_FORCE=1 \
    SCUDO_SHARED_ARENA_TRACE=0 \
    SCUDO_SHARED_ARENA_TEST_CPU=0 \
    SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=20 \
    ./scudo_shared_arena_test >/data/local/tmp/md/switch_pte_cycle_test.out 2>&1; \
    rc=\$?; \
    cat /data/local/tmp/md/switch_pte_cycle_test.out; \
    su 0 -c \"echo 0 > /sys/kernel/debug/memory_delegation/switch_cycle_stats_enabled; \
              cat /sys/kernel/debug/memory_delegation/switch_cycle_stats\"; \
    exit \$rc'" | tee "${OUT}"
echo "saved: ${OUT}"
```

也可以手动开关：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell \
   'su 0 -c \"echo reset > /sys/kernel/debug/memory_delegation/switch_cycle_stats; \
             echo enable > /sys/kernel/debug/memory_delegation/switch_cycle_stats\"'"

# ...运行 workload...

ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.4 \
  "/opt/homebrew/bin/adb shell \
   'su 0 -c \"echo disable > /sys/kernel/debug/memory_delegation/switch_cycle_stats; \
             cat /sys/kernel/debug/memory_delegation/switch_cycle_stats\"'"
```

输出列说明：

- `calls`：进入 `memory_delegation_on_context_switch()` 的次数
- `fast`：prev/next 都没有 active memory delegation ctx 的快速返回次数
- `active`：prev 或 next 有 active memory delegation ctx 的次数
- `prev_active` / `next_active`：switch-out prev / switch-in next 参与 delegation 的次数
- `drain`：检查并调用 `md_drain_log_ring()` 的次数，包含空 ring
- `drain_nonempty`：真正有 log entry 被 drain 的次数
- `syncq`：switch-in 阶段检查/尝试 queue PTE sync task_work 的次数
- `pte_sync`：实际执行 `md_sync_task_work()` 并调用 `memory_delegation_sync_mm()` 的次数
- `avg_active`：delegation hook 活跃路径的平均开销
- `avg_drain_nonempty`：非空 log ring drain 的平均开销
- `avg_pte_sync`：实际 PTE unmap/prefault 同步的平均开销
- `avg_queue_to_sync_done`：从 context switch 中成功 queue task_work 到 PTE sync 完成的平均跨度

最近一次真机参考结果：

```text
/tmp/md-switch-pte-cycle-stats-20260508-100102.txt

total:
calls=13811
active=7041
prev_active=4302
next_active=4302
drain=34401
drain_nonempty=18
syncq=4299
pte_sync=76

avg_active=63 cycles
avg_drain=9 cycles
avg_drain_nonempty=10879 cycles
avg_syncq=2 cycles
avg_pte_sync=3550 cycles
avg_queue_to_sync_done=3876 cycles

max_total=60655 cycles
max_drain=60633 cycles
max_pte_sync=9463 cycles
max_queue_to_sync_done=14537 cycles
```

若关心“有非空 log drain，并且 next 返回用户态前完成 PTE sync”的
memory delegation 额外内核路径，可粗略看：

```text
avg_drain_nonempty + avg_queue_to_sync_done
= 10879 + 3876
= 14755 cycles
```

注意：这不是完整 Linux `context_switch()` 或 syscall/yield 从进内核到出内核
的总耗时；它只覆盖 memory delegation 相关增量路径。

## 5. 手机测试输出位置

`run_phone_over_ssh_adb.sh` 默认把日志保存到本机：

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

当前手机测试已确认：

- `phone-20260507-112320`：`init_only` 通过，`MODE(init_only): pool_ready=1`
- `phone-20260507-144824`：`single-cpu revoke/free`，`SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4`，通过：
  - `PASS(scudo): shared arena malloc/free multi-proc revoke ok`
  - `PASS(scudo): shared arena retrieve/store ok`
- `phone-20260507-144904`：`cross-cpu free migration`，`SCUDO_SHARED_ARENA_TEST_CPU=0 SCUDO_SHARED_ARENA_TEST_FREE_CPU=1 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4`，通过：
  - `PASS(scudo): shared arena malloc/free multi-proc revoke ok`
  - `PASS(scudo): shared arena retrieve/store ok`

历史失败记录：

- `phone-20260507-112523`：早期版本 `single-cpu revoke/free` 在 `round=0 owner=0` 等待 child0 allocation response 时失败，用户态报 `FAIL(scudo): read(fd=14) EOF`。后续通过 Scudo fork 后 refresh/remap arena + 重新注册 log ring 修复。

## 6. QEMU 测试入口

一键 QEMU 测试：

```bash
cd ~/patent/kernel/kernel_platform/common
tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

该脚本会执行：

- 使用 `tests/Makefile` 构建 aarch64 静态测试二进制
- 生成 initramfs
- 使用 `out/Image` 启动 QEMU arm64
- 检查 serial log 中的 PASS/FAIL/panic marker

常用变量：

```bash
SMP=4
MEM=2048
QEMU_BIN=qemu-system-aarch64
KERNEL_IMAGE=~/patent/kernel/kernel_platform/out/Image
LOG=/tmp/md-qemu-run/qemu-serial.log
```

示例：

```bash
SMP=8 MEM=4096 LOG=/tmp/md-qemu-run/qemu-serial.log \
tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

QEMU 成功标志：

```text
PASS(scudo): shared arena retrieve/store ok
PASS(scudo): shared arena malloc/free multi-proc revoke ok
```

QEMU 失败筛查：

```bash
rg -n "Kernel panic|Oops|BUG:|Unable to handle|FAIL\\(scudo\\)|PASS\\(scudo\\)|memory_delegation|shared_arena" \
  /tmp/md-qemu-run/qemu-serial.log
```

## 7. QEMU 手动 Debug

手动构建测试二进制：

```bash
cd ~/patent/kernel/kernel_platform/common
make -C tools/testing/memory_delegation/tests clean all
```

手动生成 initramfs：

```bash
AUTO_EXIT=0 \
BUSYBOX=tools/testing/memory_delegation/busybox-1.36.1/busybox \
SCUDO_TEST_BIN=tools/testing/memory_delegation/tests/out/scudo_shared_arena_test \
tools/testing/memory_delegation/scripts/qemu_make_initramfs.sh /tmp/md-initramfs.cpio.gz
```

手动启动 QEMU 并保存串口日志：

```bash
RECORD_TTY=1 \
SERIAL_LOG=/tmp/md-qemu-run/qemu-serial.log \
KERNEL_IMAGE=../out/Image \
INITRAMFS=/tmp/md-initramfs.cpio.gz \
tools/testing/memory_delegation/scripts/qemu_run_aarch64.sh
```

增加内核启动日志：

```bash
APPEND_EXTRA="debug dyndbg=+p" \
tools/testing/memory_delegation/scripts/run_qemu_tests.sh
```

## 8. 测试二进制构建

Android 手机二进制：

```bash
make -C tools/testing/memory_delegation/tests MODE=android clean all
```

QEMU/Linux 静态二进制：

```bash
make -C tools/testing/memory_delegation/tests MODE=linux clean all
```

检查 ELF 类型：

```bash
file tools/testing/memory_delegation/tests/out/scudo_shared_arena_test
```

## 9. 测试程序环境变量

常用变量：

```bash
SCUDO_SHARED_ARENA_FORCE=1
SCUDO_SHARED_ARENA_TRACE=1
SCUDO_SHARED_ARENA_TEST_CPU=0
SCUDO_SHARED_ARENA_TEST_FREE_CPU=1
SCUDO_SHARED_ARENA_TEST_MODE=init_only
SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4
SCUDO_SHARED_ARENA_TEST_WARM_ITERS=8
SCUDO_SHARED_ARENA_TEST_SKIP_WARMUP=1
```

建议排查顺序：

1. `SCUDO_SHARED_ARENA_TEST_MODE=init_only`
2. `SCUDO_SHARED_ARENA_TEST_SKIP_WARMUP=1 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=1`
3. `SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4`
4. `SCUDO_SHARED_ARENA_TEST_FREE_CPU=1 SCUDO_SHARED_ARENA_TEST_MAX_ROUNDS=4`
5. 增大 rounds 做压力测试

## 10. 内核侧重点排查点

源码入口：

```text
mm/memory_delegation.c
include/linux/memory_delegation.h
include/uapi/linux/memory_delegation.h
kernel/sched/core.c
kernel/fork.c
kernel/sys.c
mm/memory.c
```

关键路径：

- `memory_delegation_arena_register()`：每 CPU arena 真值表初始化
- `memory_delegation_register_ring()`：用户态 shadow log ring 注册
- `memory_delegation_on_context_switch()`：调度边界 drain log、标记同步
- `memory_delegation_sync_mm()`：可睡眠上下文下清理 stale PTE
- `memory_delegation_fault_allowed()`：fault 时基于 `page_slot/page_owner_gen/owner_table` 判定访问是否合法
- `memory_delegation_mm_release()`：mm 退出时释放 ctx 和 owner slot

源码快速查找：

```bash
rg -n "memory_delegation_(arena_register|register_ring|on_context_switch|sync_mm|fault_allowed|mm_release|fork_mm)" \
  mm kernel include
```

## 11. 常见现象与定位方向

`init_only` 失败：

- 优先看 Scudo trace 是否创建了每个 core 的 memfd arena
- 检查 `prctl`/uapi 是否返回 `EOPNOTSUPP`、`EINVAL`、`EPERM`
- 检查 `CONFIG_MEMORY_DELEGATION` 是否开启

child pipe EOF：

- 子进程可能在 `Arena->retrieve()`、yield 后首次 touch、或者 register/fault path 中崩溃退出
- 优先看 `.out.txt` 中最后一个 child trace
- 然后看 `.logcat.txt` 是否有 tombstone、SIGSEGV、abort
- 如果可用 root，抓 `.dmesg.txt` 查 kernel Oops/panic

期望 SIGSEGV 但 touch 成功：

- 重点查 context switch 是否 drain log
- 查 `needs_pt_sync` 是否被设置并在安全上下文调用 `memory_delegation_sync_mm()`
- 查 `chunk_gen/last_seen_gen` 是否推进
- 查 stale PTE 是否真的被 unmap

合法 owner 首次 touch 反而 SIGSEGV：

- 查 `page_slot` 是否已提交为当前 mm 对应 slot
- 查 owner slot generation 是否被误复用
- 查 `memory_delegation_fault_allowed()` 对 `MD_INVALID_SLOT` 和 current slot 的判定

手机重启或 ADB 断开：

- 重新等待设备上线：
  ```bash
  ssh lrc@192.168.61.4 "/opt/homebrew/bin/adb wait-for-device"
  ```
- 检查启动原因和 pstore：
  ```bash
  ssh lrc@192.168.61.4 \
    '/opt/homebrew/bin/adb shell getprop sys.boot.reason; \
     /opt/homebrew/bin/adb shell getprop ro.boot.bootreason; \
     /opt/homebrew/bin/adb shell su 0 -c "ls -la /sys/fs/pstore"'
  ```
- 若临时 boot 丢失，重新刷当前 kernel；当前推荐用 flash 模式：
  ```bash
  cd ~/patent/kernel/kernel_platform
  ~/patent/flush_kernel.sh --mode flash
  ```
- 当前 `single-cpu 4 rounds` 和 `cross-cpu free 4 rounds` 跑完后未复现内核重启。曾观察到一次 ADB 断开/设备重启，`sys.boot.reason=reboot`、pstore 为空；加上 `WAIT_BOOT_COMPLETED=1` 后两条 4 轮用例均通过并正常返回。
