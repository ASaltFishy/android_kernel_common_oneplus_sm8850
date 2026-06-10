# SharedArena benchmark reboot/crash investigation

本文记录一次针对 Scudo SharedArena microbench 导致真机重启/崩溃的定位过程。

## 背景

测试目标是通过
`/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py`
连接跳板机，再通过 adb 在手机上运行 SharedArena latency benchmark。

测试路径：

- 跳板机：`lrc@192.168.61.230`
- 远端 adb：`/opt/homebrew/bin/adb`
- 手机测试目录：`/data/local/tmp/scudo`
- 常用 bench binary：
  `/home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/scudo_shared_arena_latency_bench`
- 需要 `--use-su`

最初计划跑单进程多线程触页版本，分配大小从 `2^4 KiB` 到 `2^18 KiB`。
完整 benchmark 在手机上稳定导致设备重启，因此转为定位重启根因。

## 初始现象

完整 benchmark 配置大致为：

```bash
--allocator combined --mode thread --touch-pages 1
--thread-counts 1,2,4,8
--sizes 16384..268435456
--iterations 80 --warmup 20 --repetitions 3
--use-su
```

现象：

- 手机在 benchmark 中途消失于 adb，约几十秒后恢复。
- `getprop ro.boot.bootreason` 显示 `reboot`。
- pstore 没有可用 panic/oops。
- dmesg 只有 OPLUS watchdog/reboot 后的启动日志，没有直接内核栈。

初始结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/phone-kernel-scudo-pow2-kib-4-18-thread-touch-su-20260518-155222
```

## 已做实验

### 1. 大内存序列复现

配置：

```text
threads=2
sizes=16,32,64,128,256 MiB
iterations=80
warmup=20
repetitions=5
touch-pages=1
```

结果：

- 多次可复现崩溃/重启。
- 崩溃位置不是固定 size：有时在 rep2 的 16MiB 附近，有时在 128MiB 附近。

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-bigseries-touch-20260518-160216
```

结论：大小不是充分必要条件，状态累积更可疑。

### 2. 单因素对照

#### 16MiB only

配置：

```text
threads=2, size=16MiB, touch=1, repetitions=5
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-16m-touch-20260518-160146
```

#### bigseries but no touch

配置：

```text
threads=2, sizes=16..256MiB, touch=0, repetitions=3
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-bigseries-notouch-20260518-161236
```

#### single thread with touch

配置：

```text
threads=1, sizes=16..256MiB, touch=1, repetitions=3
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread1-bigseries-touch-20260518-161256
```

#### 256MiB only

配置：

```text
threads=2, size=256MiB, touch=1, repetitions=3
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-256m-touch-20260518-161339
```

#### size subset

结果：

- `16,32,64,128MiB`，稳定。
- `128,256MiB`，稳定。
- `64,128,256MiB`，稳定。
- `32,64,128,256MiB`，崩溃。

相关目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-16-128-touch-20260518-161726
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-128-256-touch-20260518-162216
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-64-256-touch-20260518-162302
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-32-256-touch-20260518-162357
```

结论：size 组合只是影响跑多久和状态累积速度，不像根因。

### 3. 固定 32MiB 长跑

配置：

```text
threads=2
size=32MiB
touch=1
iterations=80
warmup=20
repetitions=20
```

结果：

- 第 19 轮附近重启。
- rep1 到 rep18 中，core0/core1 的 donate/retrieve 计数每轮增加约 51。
- `free_bad_magic=0`，用户态 freelist 没有直接显示坏 magic。
- log pending 只有小范围波动。
- MemFree/MemAvailable 没有简单线性泄漏。

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-32m-long-touch-20260518-162850
```

结论：固定大小也能靠多轮状态累积触发问题。

### 4. 固定 32MiB 不触页长跑

配置：

```text
threads=2
size=32MiB
touch=0
repetitions=30
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-32m-long-notouch-20260518-163303
```

结论：只做 alloc/free/log 累积不够，需要真实触页进入 page fault/PTE 路径。

### 5. 单进程长跑

配置：

```text
threads=2
size=32MiB
touch=1
iterations=1800
warmup=20
repetitions=1
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-32m-singleprocess-1800-touch-20260518-163744
```

结论：

- 不是同一个 mm 内长时间 fault/PTE sync 就必然坏。
- 多个 benchmark 进程反复退出/重进更关键。
- 怀疑点转向 `mm_release`、ctx 生命周期、常驻 broker backing 状态复用。

### 6. 传统 Scudo 路径对照

配置：

```text
SharedArena ready=0
traditional secondary path only
threads=2
size=32MiB
touch=1
repetitions=20
```

结果：稳定。

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/control-traditional-thread2-32m-touch-retry-20260518-164137
```

结论：普通进程 churn + 多线程触页不是问题，问题在 SharedArena/memory delegation 路径。

### 7. 每轮重启 broker

配置：

```text
threads=2
size=32MiB
touch=1
iterations=80
warmup=20
repetitions=25
每一轮 benchmark 前 kill 并重新启动 memory_delegation_broker
```

结果：

- 25 轮完成，没有手机重启。
- 有一轮 attach 过早导致 `SharedArenaPool not ready`，视为无效轮，不作为稳定性证据。
- 其余 delegated 轮正常。
- 每轮 core0/core1 的 donate/retrieve 都回到 51 左右，不再跨轮累积。

结论：

- 崩溃不是“累计触页 N 次”本身。
- 常驻 broker 让 backing arena header/freelist 跨进程保存，是触发条件的重要组成。
- fresh broker 会重置用户态 backing 状态，因此稳定。

### 8. 常驻 broker + 客户端 reset-on-init 试验

为了区分“broker 进程本身状态”与“broker backing arena 状态”，临时加入环境变量：

```text
SCUDO_SHARED_ARENA_RESET_ON_INIT=1
```

实现位置：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/shared_arena_linux.cpp
```

含义：

- 默认不改变行为。
- 开启后，客户端 attach/register 成功后调用现有 `SharedArena::reset()`，尝试清空用户态 arena header/freelist/log ring。

配置：

```text
常驻 broker
threads=2
size=32MiB
touch=1
iterations=80
warmup=20
repetitions=25
SCUDO_SHARED_ARENA_RESET_ON_INIT=1
```

结果：

- 没有复现手机重启。
- 第 17 轮客户端进程自身 SIGSEGV，退出码 139。
- tombstone 显示：

```text
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR)
fault addr 0x0000001020262000
```

addr2line：

```text
0x2f084 -> touchAllocatedPages()
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/tests/shared_arena_latency_bench.cpp:268
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-thread2-32m-persistent-reset-on-init-20260518-1705
```

关键观察：

- rep17 raw 起始处显示 arena header 已经被 reset：

```text
bump_pages=0
free_blocks=0
donated=0
retrieved=0
donate_count=0
retrieve_count=0
```

- 崩溃发生在触页阶段，不是 freelist scan 阶段。
- dmesg 中该进程 `mm_release` 最后释放了 `cpu=1 released_pages=8193`，说明至少部分 owner 元数据仍存在并参与释放。

结论：

- 只重置客户端看到的 header/freelist 并不等价于 fresh broker。
- 这会让客户端从 bump 重新拿地址，但内核侧 arena ownership/chunk 元数据可能仍带有旧状态。
- 旧 owner 与新进程分配地址冲突时，触页可能被 fault hook 拒绝，表现为用户态 SIGSEGV。
- 原始不 reset 的重启问题可能是同类“用户态 backing 状态 vs 内核 owner 元数据不同步”进一步恶化后的结果。

### 9. `mm_release` freelist 发布顺序修复尝试

基于代码检查发现一个可能窗口：

```text
memory_delegation_mm_release()
  1. 扫描退出 mm 仍拥有的 owner runs。
  2. 将这些 run 插入用户态 SharedArena freelist。
  3. 释放用户态 arena lock。
  4. 再清理内核 page_slot/page_free/page_owner_gen/chunk_gen/delta。
```

风险：

- 其他进程可能在步骤 2 和步骤 4 之间从 freelist 取走该 VA。
- 此时用户态认为该块已空闲可分配，但内核 truth 仍属于退出 mm。
- 新进程后续 alloc log 可能因 `page_slot != MD_INVALID_SLOT` 被 `-EBUSY` 拒绝，或者触页时被 ownership fault 拒绝。

临时修复：

- 修改 `mm/memory_delegation.c` 中 `memory_delegation_mm_release()`。
- 将 `md_user_arena_unlock(user_lock_page, arena_base)` 后移到插入成功的 run 已经完成内核 owner metadata 清理并提交 `chunk_gen/delta` 之后。
- 目标是让其他进程只有在用户态 freelist 与内核 truth 都完成更新后，才能看见这些释放块。

构建与刷机：

```bash
cd /home/lrc/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
~/patent/flush_kernel.sh --remote lrc@192.168.61.230
```

结果：

- 内核构建成功。
- `flush_kernel.sh --remote lrc@192.168.61.230` temp boot 成功。
- 启动 broker 后运行固定 32MiB 复现：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 33554432 \
  --iterations 80 \
  --warmup 20 \
  --repetitions 20 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --arena-only 1" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-mm-release-atomic-freelist-32m-20rep-20260520
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-mm-release-atomic-freelist-32m-20rep-20260520
```

观察：

- 第 1 轮运行过程中 raw 输出显示 benchmark 已经进入真实触页路径。
- 最后记录停在触页/释放交错阶段附近，例如：

```text
phase=before_touch kind=measure iter=8 cpu=0 ptr=0x1000002000
```

- 随后 adb 设备消失。
- 约 94 秒后设备重新出现。
- `uname -a` 回到旧 boot kernel：

```text
Linux localhost 6.12.23-android16-5-o-g6429f562cb8c-4k #1 SMP PREEMPT Thu May 14 09:02:24 UTC 2026 aarch64 Toybox
```

说明 temp boot 的新内核已经重启并回落到原 boot。

结论：

- `mm_release` 中“freelist 先发布、内核 truth 后清理”的窗口是真实风险，修复方向仍然合理。
- 但该单点修复不足以解决当前真机重启。
- 根因更可能还包含普通 `SharedArena::store()` 释放路径中的同类窗口：用户态 freelist 已经更新，但对应 `FREE` log 尚未被内核 drain/commit，其他线程或后续进程就可能重新 retrieve 同一块。
- 后续应继续检查“用户态 freelist 变更”和“内核 page ownership 变更”之间是否存在更普遍的非原子发布问题，而不能只盯 `mm_release`。

## 当前代码观察

### 用户态 SharedArena

Android 上当前逻辑：

- 应用进程只连接常驻 `memory_delegation_broker` 获取 backing fd。
- 每个进程 attach 同一份 broker backing。
- Linux 路径有 `shm_unlink` 清理旧 session 的共享内存，但 Android attach 路径不会自动清空 backing。
- `SharedArena::reset()` 存在，但默认不会在 Android attach 后调用。

重要代码点：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/shared_arena_linux.cpp
SharedArena::init()
SharedArena::reset()
SharedArena::store()
SharedArena::retrieve()
SharedArenaPool::init()
```

### 内核 memory_delegation

重要代码点：

```text
mm/memory_delegation.c
md_reset_all_arenas_if_idle()
memory_delegation_register_ring()
md_drain_log_ring()
md_apply_log_entry_locked()
md_memory_delegation_sync_mm()
memory_delegation_mm_release()
md_fault_addr_allowed()
memory_delegation_fault_allowed()
```

#### per-CPU arena 的两层生命周期

`md_arena_meta` 结构体（含 `page_slot`、`page_owner_gen`、`chunk_gen`、`delta_runs` 等数组）在第一个进程调用 `register_ring()` 时由 `memory_delegation_arena_register()` 分配，此后**永久驻留内核**，不随进程退出释放。只有显式调用 `memory_delegation_arena_unregister()` 才会释放，正常运行中不会触发。

arena 内容（ownership 元数据）有独立的重置时机，由 `md_reset_all_arenas_if_idle()` 负责，条件是 `md_active_mms == 0`，调用点仅在 `memory_delegation_register_ring()` 内。

#### `mm_release` 已经处理的字段

`memory_delegation_mm_release()` 在进程退出时会正确清理 ownership 字段：

- `page_slot[page] = MD_INVALID_SLOT`
- `page_owner_gen[page] = 0`
- `page_free[page]` 更新
- `owner_table[slot].valid = false`（通过 `md_put_owner_slot_locked`）

因此，如果 `mm_release` 正确执行完毕，**ownership 相关数组在下一轮进程注册前已经干净**，不需要 reset 来补救。

#### `md_reset_all_arenas_if_idle()` 真正必要的字段

`mm_release` 不会清理以下字段，这才是 reset 的实际必要性所在：

| 字段 | 不清理的后果 |
|------|-------------|
| `global_commit_seq` | 新进程 ctx 的 `last_seen_gen` 从 0 开始，而 commit_seq 已是上一轮的高值，触发对所有旧 delta 的全量重放 |
| `chunk_gen[]` | chunk scan 路径用 `chunk_gen > last_seen` 判断脏块，旧值导致新进程做不必要的全量扫描 |
| `delta_runs[]` | 旧 delta 记录（`REVOKE_NON_OWNER`/`PREFAULT_FREE`）被新进程的 PTE sync 重放，造成错误 unmap |

具体路径：`md_memory_delegation_sync_mm()` 中：

```c
target_seq = READ_ONCE(arena->global_commit_seq); // 旧值，如 500
last_seen  = ctx->last_seen_gen;                  // 新进程，= 0

// md_sync_mm_from_deltas 过滤：run->seq > 0 && run->seq <= 500
// → 上一代所有 delta_runs 全部命中，被错误重放到新进程
```

重放 `REVOKE_NON_OWNER` 时，旧 `owner_table` 已被 `mm_release` 清零（`valid=false`），`md_delta_run_owner_is_mm()` 返回 false，新进程的 PTE 被 unmap，后续触页进入 fault 路径，与真实 ownership 产生不一致。

关键行为：

- `memory_delegation_register_ring()` 中，当 `md_active_mms == 0` 时会调用 `md_reset_all_arenas_if_idle()` 重置内核 arena 元数据。
- `memory_delegation_mm_release()` 会：
  - drain 当前 mm 的 log ring。
  - 扫描内核 arena 中属于该 mm 的 owner pages。
  - 将遗留 owner runs 插入用户态 arena freelist。
  - 清除内核 owner metadata（page_slot / owner_table）。
  - 删除该 mm 的 ctx，并减少 active mm 计数。
- fault 路径中，若页属于其他 mm，会返回 `VM_FAULT_SIGSEGV`。

## 当前判断

最可能的方向：

```text
常驻 broker backing 中的用户态 arena header/freelist
与
内核侧全局 per-cpu arena owner/chunk/delta 元数据
在多进程轮次之间出现不同步。
```

触发条件目前看起来是：

1. SharedArena delegated path。
2. 多个 benchmark 进程反复进入/退出。
3. 常驻 broker 复用同一份 backing arena。
4. 真实触页，进入 fault/PTE sync 路径。
5. 单进程长跑和传统 Scudo 路径均不触发。

不支持的假设：

- 单纯大内存导致崩溃：已被 32MiB fixed long-run 和 256MiB only 稳定结果削弱。
- 单纯 drain log 性能问题：no-touch 长跑稳定，且 log pending 没有异常膨胀。
- 单纯用户态 freelist 结构坏：多轮崩溃前 `free_bad_magic=0`，但不能完全排除更隐蔽的不一致。
- 单纯修复 `mm_release` 遗留 owner run 的 freelist 发布窗口不足以消除重启：第 9 组实验仍在第 1 轮触发设备重启。

## 2026-05-20 baseline retry 新观察

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-seqcount-fault-full-pow2-kib-4-18-thread-touch-su-with-baseline-retry-20260520-184841
```

现象：

- `rep1 threads=1` 完整跑完，delegated + traditional 均有结果。
- `rep1 threads=2` 在 `size=8MiB` 的 `traditional (cache/mmap)` touch 阶段附近设备重启。
- raw 最后一条停在：

```text
traditional (cache/mmap), size=8388608, phase=before_alloc, kind=measure, iter=20
```

关键新线索：

- 在进入 `size=8MiB` traditional 之前，`core=1` 的用户态 log ring 已经积压并溢出：

```text
log_head=0 log_tail=338 log_pending=338 log_dropped=35
```

- 338 基本就是当前一页 log ring 的最大 entry 容量。也就是说，baseline 崩溃前已经出现“delegated 阶段产生的 alloc/free log 没有及时被内核 drain，随后 ring 满并发生 dropped”的状态。
- `traditional` 路径本身没有从 SharedArena 分配；但进程已经 attach/register 了 SharedArena，普通 mmap touch 会产生大量普通缺页和调度/返回用户态机会，从而触发 active delegation mm 的 `task_work`/PTE sync/drain。它更像是暴露前序积压状态的触发点，不是直接根因。

当前更强判断：

```text
根因从“traditional baseline 自身有问题”
收敛为
“多线程 delegated 阶段的 per-(mm,cpu) log ring 背压不足，ring 满后出现 dropped/延迟提交；
后续 traditional 阶段触发 drain/sync，把不完整或过期的 ownership 状态带入 PTE/fault 生命周期”。
```

这也解释了为什么 `--arena-only 1` 可以更稳定：少了 baseline 阶段的大量普通 mmap/touch fault，触发后续 PTE sync/drain 的时机和压力不同；而 `--arena-only 0` 会稳定把问题带回到 “threads=2 + 前序 delegated log 积压 + traditional touch” 的组合。

新的修复优先级：

1. 先解决 log ring 背压，避免 `log_dropped` 在正确性路径出现。
   - 当前一页 ring 容量太小，`threads=2` 下几个 size 的 delegated 阶段就能积压到满。
   - 仅 `sched_yield()` 一次不可靠；如果 pinned worker 没有真正切出，内核不会及时 drain。
   - 正确方向是 ring 接近满时提供同步 drain/submit 慢路径，或者扩大 ring 并让内核支持 pin 多页 ring。
2. 修复 arena block `deallocate()` 的失败语义。
   - 当前 `Owner->store()` 失败后，allocator 已经把块从 `InUseBlocks` 移除，然后直接返回。
   - 这不会把块放回 freelist，也不会提交 FREE log；内核仍认为它属于当前 mm，只能等 `mm_release` 回收。
   - 这至少会制造长生命周期 ownership 残留，并放大 `mm_release`/PTE sync 压力。正确实现应保证 FREE log 可提交后再发布 freelist，或在 log 满时同步 drain 并重试，不能静默丢弃释放。
3. 保留 `last_seen_gen = global_commit_seq` 与 fault 侧 seqcount 修复。
   - 它们仍然是必要修复，但这次 baseline retry 表明，仅靠跳过历史 delta 和 fault 读一致性还不够。

## 2026-05-20 delegated-only 复现补充

为确认 baseline/traditional 不是唯一触发源，重新跑 delegated-only 路径：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 --warmup 20 --repetitions 5 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path delegated" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-delegated-only-thread2-full-pow2-touch-freshbroker-20260520
```

结果：

- rep1/rep2/rep3 完整完成。
- rep4 在 `size=16MiB` warmup 触页阶段设备重启。
- raw 停点：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-delegated-only-thread2-full-pow2-touch-freshbroker-20260520/raw/threads02_rep04.txt
```

停点附近：

```text
size=16777216 phase=after_alloc  kind=warmup iter=0 cpu=0 ptr=0x101efed000
size=16777216 phase=before_touch kind=warmup iter=0 cpu=0 ptr=0x101efed000
size=16777216 phase=after_alloc  kind=warmup iter=0 cpu=1 ptr=0x103edec000
size=16777216 phase=before_touch kind=warmup iter=0 cpu=1 ptr=0x103edec000
size=16777216 phase=after_touch  kind=warmup iter=0 cpu=0 ptr=0x101efed000
```

adb 恢复后 `/proc/uptime` 约 45 秒，确认是重启级事件。

关键观察：

- delegated-only 自身会触发 ring drop，不依赖 traditional baseline。
- full pow2 序列中，第一次明显 drop 可在 rep2 的 `2MiB` 阶段附近看到：

```text
core=1 ... free_bytes=534687744 donated=27510964224 retrieved=27513065472
donate_count=832 retrieve_count=833 log_head=103 log_tail=441
log_pending=338 log_dropped=35
```

- rep4 崩溃前，core1 仍保持不平衡：

```text
core=1 ... free_bytes=534687744 donated=82874097664 retrieved=82876198912
donate_count=2150 retrieve_count=2151 log_head=711 log_tail=711
log_pending=0 log_dropped=2
```

这说明问题不是 baseline 把正常 delegated 状态“撞坏”，而是 delegated-only
在 full pow2 + threads=2 + touch 下已经会进入 log ring 丢记录和
donate/retrieve 不平衡状态；baseline 只是更稳定地暴露后续 PTE/fault 压力。

为了缩小触发条件，又跑了两个安全复现：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-delegated-only-thread2-1-2m-trace-20260520-continue
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/repro-delegated-only-thread2-up-to-2m-trace-20260520-continue
```

结论：

- 只跑 `1MiB,2MiB` 时 pending 只到约 203，未复现 drop。
- 跑完整小尺寸序列到 `2MiB` 时，可以复现 `log_pending=338/log_dropped=35`。
- 但如果没有先跑完整大尺寸序列把 arena 扩到满容量，通常不会马上出现
  donate/retrieve 不平衡；重启依赖“ring drop + 满 arena freelist 状态 + 后续大块触页”的组合。

当前更具体的根因判断：

1. 用户态每个进程每个 CPU 的 shadow log ring 只有一页，容量约 338 条。
2. `appendLogPagesLocked()` 在 ring 满时只 `sched_yield()` 一次；如果当前 pinned worker
   没有真正切出到能让内核 drain 的路径，就会递增 `Dropped` 并返回 false。
3. `SharedArena::retrieve()` 对 alloc log 失败有回滚，因此可能只表现为 drop。
4. `MapAllocator::deallocate()` 已经先从 `InUseBlocks` 移除；随后 `Owner->store()` 如果因为
   FREE log append 失败而返回 false，当前代码只打印 `skip-shared-return` 并 return，块不会进入
   freelist，也没有对应 FREE log。这个路径会制造 `retrieve_count > donate_count` 和
   owner 残留，后续只能靠 `mm_release` 补救。

因此下一步修复优先级应提升为：

- 正确性路径禁止静默丢 shadow log。ring 满时需要同步 drain/submit 慢路径、可阻塞重试，
  或把 ring 扩展为内核可 pin 的多页 ring。
- `deallocate()` 不能在 `Owner->store()` 失败后把块从 allocator 生命周期里丢掉；
  必须保证 FREE log 可提交后再发布 freelist，或同步 drain 后重试。

## 建议下一步

### 1. 加内核定位日志

建议临时加 `pr_info` 或 tracepoint，重点不是性能而是确认状态机。

建议打点：

- `md_reset_all_arenas_if_idle()`
  - 是否执行 reset
  - reset 前后 `md_active_mms`
  - 每个 cpu arena 的 `global_commit_seq`
- `memory_delegation_register_ring()`
  - `mm`
  - `cpu`
  - `arena_base`
  - `nr_pages`
  - `ctx->last_seen_gen`
  - register 前后 active 状态
- `memory_delegation_mm_release()`
  - drain 前后 ring head/tail
  - per-cpu run_count、inserted_runs、released_pages
  - 删除 ctx 前 active_mms
- `md_fault_addr_allowed()` / `memory_delegation_fault_allowed()`
  - 只在 deny 时打印
  - `mm`
  - `cpu`
  - `address`
  - `page_idx`
  - `slot`
  - `owner_mm`
  - `owner_gen`
  - `page_owner_gen`

这些日志应该能直接验证：

- 第 N 轮 register 是否真的 reset 了内核元数据。
- SIGSEGV 的 `0x1020262000` 是否被判定为旧 owner。
- 原始重启前是否有释放/注册/ctx 状态异常。

### 2. 根因修复方向

#### 2a. 消除对 `md_reset_all_arenas_if_idle()` 的依赖

分析表明 `md_reset_all_arenas_if_idle()` 真正必要的场景只有一个：防止新进程的 PTE sync 重放上一代的 `delta_runs`。根因是新 ctx 的 `last_seen_gen` 初始化为 0，而 `global_commit_seq` 保留了上一代的历史值。

**最小修复**：在 `memory_delegation_register_ring()` 中，完成 `md_mm_ctx_activate()` 之后，将新 ctx 的 `last_seen_gen` 初始化为当前 `global_commit_seq`：

```c
// register_ring() 中 activate 之后：
WRITE_ONCE(ctx->last_seen_gen, READ_ONCE(arena->global_commit_seq));
```

含义：新进程从”现在”开始观察 arena 变化，不重放历史 delta。此后：

- `delta_runs` 不再需要 reset（新进程天然跳过旧条目）。
- `global_commit_seq` / `chunk_gen` 不再需要回零（新进程 `last_seen == target_seq`，首次 sync 直接跳过）。
- `page_slot` / `owner_table` 已由 `mm_release` 正确清理，无需 reset 兜底。

这样 `md_reset_all_arenas_if_idle()` 可以被完全移除，arena 元数据内容的唯一维护者变为 `mm_release`，职责清晰。

#### 2b. 修复 `mm_release` 中 freelist 发布窗口（竞态补丁，独立于 2a）

当前 `mm_release` 对每个 CPU 的操作顺序（见 `memory_delegation_mm_release()`）：

```
1. md_user_arena_lock()              ← 获取用户态 arena 锁
2. md_user_freelist_insert_locked()  ← 插入 freelist（其他进程此时可见）
3. spin_lock(&arena->lock)
4. 清除 page_slot / page_owner_gen   ← 内核 truth 更新
5. md_commit_dirty_chunks_locked()
6. spin_unlock(&arena->lock)
7. md_user_arena_unlock()            ← 释放用户态 arena 锁
```

步骤 2 完成后、步骤 4 完成前，其他进程可以从 freelist 取走该 VA，写入 `MD_LOG_ALLOC`，drain 时遭遇 `page_slot != INVALID_SLOT` 返回 `-EBUSY`，用户态误认为拥有该块，触页时被 fault hook 拒绝。

修复方向：将 `md_user_arena_unlock()` 后移到内核 truth 清理（步骤 4–6）完成之后，确保 freelist 可见性与内核 ownership 清除对其他进程是原子的。

#### 2c. 验证普通 `store()` 路径的同类窗口

`mm_release` 是进程退出时的被动清理；主动路径 `SharedArena::store()`（用户 free）也存在类似窗口：用户态 freelist 先更新，对应 `MD_LOG_FREE` 尚未被内核 drain/commit，另一个线程即可 retrieve 同一块。若确认存在，需要在 `store()` 完成后等待内核 commit 确认，或引入 pending 标记阻止二次 retrieve。

### 3. 谨慎处理 `RESET_ON_INIT`

`SCUDO_SHARED_ARENA_RESET_ON_INIT=1` 只是定位开关，不应直接作为修复。

原因：

- 它只改用户态 header/freelist/log ring。
- 若内核 metadata 没有同步 reset，反而会制造新的不一致。
- 第 17 轮 SIGSEGV 已经说明这个方案不完整。

## 临时改动提醒

本次定位过程中在 Scudo 侧加入过临时开关：

```text
SCUDO_SHARED_ARENA_RESET_ON_INIT
```

文件：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/shared_arena_linux.cpp
```

如果后续要提交正式修复，应重新评估是否保留该开关，或者改成更严格的 generation/reset 协议。

## 2026-05-20 seqcount fault 快照修复复测

在修复 drain log 不完整问题、用户态 Scudo 改为无论归属都写本 CPU log 后，完整
Python benchmark 不再触发设备重启，但曾在第 3 轮 `threads=1,size=8388608`
处触发 `SIGSEGV`。对应 deny 日志显示 fault 页的 `owner_mm` 与当前进程
`mm` 相同：

```text
deny arena access pid=13019 comm=scudo_shared_ar
addr=0x101f8b1000
owner_mm=000000001d3fe35d
owner_gen=5164
page_owner_gen=5164
owner_ref_pages=2049
page_free=0
```

这说明 fault deny 更像是 `md_fault_addr_allowed()` 无锁读取
`page_slot/page_owner_gen/owner_table` 时撞上 ownership 提交窗口，读到了不一致快照，
而不是页面真的被其他进程拿走。

当前内核修复没有在 fault 路径持有 `arena->lock`，而是在 `md_arena_meta` 中新增
`ownership_seq`：

- 写侧继续在既有 `arena->lock` 保护下修改 ownership 真值表，并用
  `write_seqcount_begin/end(&arena->ownership_seq)` 包住提交窗口。
- fault 读侧使用 `read_seqcount_begin/retry()` 无锁读取一致快照，再判断
  `page_slot/page_owner_gen/owner_table`。
- 这样同时满足 `DESIGN.md` 中 fault 路径不引入 `spinlock/mutex` 的约束，并避免读撕裂误拒绝。

验证步骤：

```bash
cd ~/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
~/patent/flush_kernel.sh
```

刷机后需要重新启动常驻 broker，否则 benchmark 会显示 `SharedArena: ready=0`：

```bash
cd /data/local/tmp/scudo
rm -f broker.log broker.pid
./memory_delegation_broker --trace >broker.log 2>&1 &
grep -q "ready num_cores=" broker.log
```

完整复测命令等价于：

```bash
python3 run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 --warmup 20 --repetitions 3 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1"
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-seqcount-fault-full-pow2-kib-4-18-thread-touch-su-ready-20260520-171238
```

结果：

- 完整跑完 3 轮、4 组线程数、15 个 size，`raw_results.csv` 共 720 条记录。
- 已越过此前崩溃点：第 3 轮 `threads=1,size=8MiB`。
- raw 日志未出现 `SIGSEGV`、`Fatal signal`、`delegated skipped` 或 `SharedArena: ready=0`。
- dmesg 关键字扫描未发现 `deny arena access`、kernel oops 或 panic。
- 设备未重启；刷机后的 boot reason 为 `bootloader`，复测后 uptime 正常递增。

结论：本轮结果支持“fault 路径无锁读取 ownership 真值表时读到不一致快照导致误拒绝”的判断。
seqcount 一致性快照修复消除了当前可复现的 `SIGSEGV`，并且没有重新引入设备重启。

## 2026-05-20 连续完整 benchmark 5rep 复测

为了把上一轮 3rep 通过结果升级为连续稳定性证据，继续跑同一完整矩阵，但将
`--repetitions` 从 3 提高到 5：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 --warmup 20 --repetitions 5 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-seqcount-fault-full-pow2-kib-4-18-thread-touch-su-5rep-20260520-200811
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-seqcount-fault-full-pow2-kib-4-18-thread-touch-su-5rep-20260520-200811
```

结果：

- `rep1 threads=1` 完整完成，脚本解析出 60 条记录。
- `rep1 threads=2` 运行到最大 size `256MiB` 的 delegated 阶段后设备从 adb 消失，
  Python wrapper 返回 adb/ssh exit status 255。
- 设备约 70 秒后重新出现在 adb。
- 恢复后：

```text
getprop ro.boot.bootreason = reboot
/proc/uptime ~= 133s
uname -a = Linux localhost 6.12.23-android16-5-o-g576abba954b4-4k #1 SMP PREEMPT Mon May 18 07:33:08 UTC 2026 aarch64
```

- `/sys/fs/pstore` 为空。
- 恢复后的 dmesg 未保留崩溃前 kernel oops/panic 栈，只能看到启动后的
  OPLUS watchdog/hung task 相关日志以及大量普通进程 `mm_release had_active=0` 日志。

raw 停点：

```text
raw/threads02_rep01.txt

size=268435456 phase=before_alloc kind=measure iter=3 cpu=0 ptr=0x0
size=268435456 phase=after_alloc  kind=measure iter=3 cpu=0 ptr=0x100ff0a000
size=268435456 phase=before_touch kind=measure iter=3 cpu=0 ptr=0x100ff0a000
```

更早的同一 `threads=2` raw 中，进入 `256MiB` 前的 arena 状态：

```text
core=0 bump_pages=130825 free_blocks=1 free_bad_magic=0
       donated=68410986496 retrieved=68410986496
       donate_count=2701 retrieve_count=2701
       log_head=1121 log_tail=1122 log_pending=1 log_dropped=0

core=1 bump_pages=65288 free_blocks=1 free_bad_magic=0
       donated=14754234368 retrieved=14754234368
       donate_count=1596 retrieve_count=1596
       log_head=1122 log_tail=1122 log_pending=0 log_dropped=0
```

补充观察：

- `threads=2` 崩溃前没有看到 `free_bad_magic`，donate/retrieve 也保持平衡。
- `threads=2` 崩溃前没有明显 log drop；这和此前“ring drop + ownership 不完整”
  的方向不同，至少本轮停点不支持把直接触发归因于当前进程的 log ring drop。
- 但前一个已经完成的 `threads=1` 轮里，core0 曾出现：

```text
log_head=0 log_tail=338 log_pending=338 log_dropped=131
```

  说明一页 shadow log ring 在完整 pow2 序列中仍会进入 drop 状态；只是本轮
  `threads=1` 仍可完整完成，drop 不再等价于立刻 SIGSEGV/重启。

结论更新：

- `ownership_seq` 修复仍然成立：它解释并修复了 fault 路径读到不一致
  ownership 快照导致的误 `SIGSEGV`。
- 但“完整矩阵 3rep 通过”不能作为最终稳定性结论。5rep 压力下仍能触发设备
  重启，最新触发点是 `threads=2,size=256MiB,delegated,touch`。
- 当前 reboot 更像另一个问题：在 arena 已被前序 full pow2 序列扩到接近满容量后，
  两线程同时触摸 256MiB delegated 块，触发极重 fault/PTE/page 分配或回收路径；
  设备直接重启且 pstore 为空，因此还需要进一步拆分“内存压力/watchdog”与
  “memory delegation ownership/PTE 同步”两类可能性。

建议下一步收敛实验：

1. fresh broker / fresh boot 后单独跑 `threads=2,size=256MiB,touch=1`，确认
   256MiB 双线程本身是否稳定。
2. 跑完整 pow2 前缀到 `128MiB` 后，只跑 `256MiB`，确认是否需要 arena
   被扩到接近满容量才触发。
3. 对比 `--path delegated`、`--arena-only 1`、`touch=0` 三组，区分 SharedArena
   ownership/PTE 路径和单纯大内存触页压力。
4. 复测时打开更聚焦的 debugfs 统计或 trace，但先减少 `mm_release had_active=0`
   这类启动后噪声日志，否则重启后的 dmesg 有效信息会被冲掉。

## 2026-05-20 bump alloc prefault 根因定位

继续按上一节的收敛实验拆分触发条件。

### 1. fresh broker 单点 256MiB

fresh broker 后只跑单个 size：

```bash
python3 run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 268435456 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path delegated" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-256m-only-thread2-delegated-touch-freshbroker-20260520-201655
```

结果：稳定完成。

结论：`threads=2,size=256MiB,touch=1` 本身不是充分触发条件。

### 2. fresh broker + pow2 前缀到 128MiB

fresh broker 后跑 `16KiB..128MiB`：

```bash
python3 run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path delegated" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-prefix-to-128m-thread2-delegated-touch-20260520-201723
```

结果：

- 设备再次从 adb 消失，约 78 秒后恢复。
- 恢复后 `/proc/uptime ~= 47s`，确认重启级事件。
- pstore 为空，dmesg 未保留崩溃前栈。
- raw 停点提前到 `64MiB`：

```text
size=67108864 phase=before_alloc kind=measure iter=0 cpu=0 ptr=0x0
```

进入 `64MiB` 前 arena 状态：

```text
core=0 bump_pages=16361 free_bad_magic=0
       donated=3417747456 retrieved=3417747456
       donate_count=459 retrieve_count=459
       log_head=918 log_tail=918 log_pending=0 log_dropped=0

core=1 bump_pages=16361 free_bad_magic=0
       donated=3417747456 retrieved=3417747456
       donate_count=459 retrieve_count=459
       log_head=918 log_tail=918 log_pending=0 log_dropped=0
```

结论：

- 最新触发点不需要 256MiB，前序 pow2 touched 序列进入 64MiB 就能触发。
- 直接触发前没有 `free_bad_magic`、donate/retrieve 不平衡或当前进程 log drop。
- 因此这轮不支持“freelist 损坏”或“当前 ring drop 直接导致 ownership 不完整”。

### 3. 同样前缀但 `touch-pages=0`

```bash
python3 run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 0 --path delegated" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-prefix-to-128m-thread2-delegated-notouch-20260520-202031
```

结果：稳定完成。

结论：必须经过真实触页 / page fault / PTE 同步路径；纯 alloc/free/log/freelist
路径不足以触发。

### 4. fresh broker 单点 64MiB

```bash
python3 run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 67108864 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path delegated" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-64m-only-thread2-delegated-touch-freshbroker-20260520-202054
```

结果：稳定完成。

结论：`64MiB` 本身不是充分条件；需要前序 touched delegated 序列带来的
arena/PTE 状态累积。

### 5. 根因代码路径

用户态 `SharedArena::retrieve()` 原先在 bump 分配时给 `ALLOC` log 打
`kLogFlagPrefault`：

```c++
const u16 LogFlags = FromBump ? kLogFlagPrefault : 0;
appendLogLocked(SharedArenaLogOp::Alloc, AllocBase, AllocSize, LogFlags)
```

这和设计里的 bump 语义冲突：DESIGN.md §2.2 明确要求首次 bump 分配的页面
“尚无物理页，通过实际访问时的 page fault 按需提交”。

内核侧对应路径：

```c
// md_apply_log_entry_locked()
prefault = (entry->flags & MD_LOG_F_PREFAULT) != 0;
memset(arena->page_prefault + start, prefault, nr_pages);

// md_snapshot_chunk_state()
prefault_pages[...] =
  (arena->page_slot[page] == MD_INVALID_SLOT && arena->page_free[page]) ||
  (arena->page_prefault[page] && md_page_belongs_to_mm_locked(...));

// md_memory_delegation_sync_mm()
md_prefault_range_locked(mm, start, end);
```

`md_prefault_range_locked()` 会在 `mmap_write_lock(mm)` 下对整段执行
`get_user_pages_remote(..., FOLL_WRITE, ...)`。因此原先的 bump ALLOC log 会让
新 bump 块在 PTE sync/task_work 中被整段预缺页，等价于把用户态按需触页转成
内核返回用户态前的大范围同步 prefault。

这解释了实验矩阵：

- no-touch 稳定：缺少真实 fault / 调度返回 / PTE sync 压力。
- 64MiB/256MiB 单点稳定：没有足够前序状态累积。
- pow2 touched 前缀触发：前序 size 让 arena 和 PTE sync 进入持续变更状态，
  后续中大块 bump/free/retrieve 使整段 prefault、unmap、fault 交错，最终在
  设备侧表现为 watchdog/reboot 级事件，且 pstore 无栈。

### 6. 实验修复验证

临时修改：

```diff
-  const u16 LogFlags = FromBump ? kLogFlagPrefault : 0;
+  const u16 LogFlags = 0;
```

同时移除 `FromBump` 临时变量，使 bump 分配不再请求内核整段 prefault。

重新编译并部署 benchmark 后复测：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-disable-bump-prefault-prefix-to-128m-thread2-touch-20260520-202242
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-disable-bump-prefault-full-thread2-touch-20260520-202310
```

结果：

- `threads=2,16KiB..128MiB,delegated,touch` 完整完成。
- `threads=2,16KiB..256MiB,delegated,touch` 完整完成。
- 设备未重启，恢复后的 uptime 持续递增。

当前结论：

```text
根因是用户态 bump 分配错误设置 kLogFlagPrefault，
导致内核把本应 demand-fault 的首次分配页在 PTE sync/task_work 中整段 prefault。
在多线程 touched pow2 序列中，这会制造大量同步 GUP/PTE 工作和物理页提交压力，
最终触发设备 reboot/watchdog 级故障。
```

正式修复方向：

- bump 分配的 `ALLOC` log 不应携带 `MD_LOG_F_PREFAULT`。
- 保留 free 路径只 prefault freelist header 页的逻辑；这是为了让 allocator
  后续扫描 free metadata，不等价于整段预提交。
- 后续可考虑移除 `MD_LOG_F_PREFAULT` 在 alloc 语义中的使用，或把该 flag
  改名/限制为“metadata prefault”，避免再次误用于大块数据页。

## 2026-05-20 full benchmark both-path 复测

### 1. 运行配置

在移除 bump allocation 的 `kLogFlagPrefault` 后，重新启动常驻 broker，并运行
包含 delegated 和 traditional baseline 的全量 benchmark：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 --warmup 20 --repetitions 3 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path both" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-disable-bump-prefault-full-pow2-kib-4-18-thread-touch-both-3rep-20260520-203343
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-disable-bump-prefault-full-pow2-kib-4-18-thread-touch-both-3rep-20260520-203343
```

### 2. 结果

- `rep1 threads=1` 完整完成，`parsed_records=120`。这符合
  `15 sizes * 2 paths * 4 metrics`，说明 delegated 和 traditional 都被纳入。
- `rep1 threads=2` 过程中设备从 adb 消失，约 78 秒后恢复。
- 恢复后：

```text
ro.boot.bootreason = reboot
/proc/uptime = 45.80 338.14
kernel = Linux localhost 6.12.23-android16-5-o-g576abba954b4-4k
```

- `/sys/fs/pstore` 仍为空。
- 重启后的 `dmesg` 没有保留崩溃前栈，只有 boot 后大量
  `memory_delegation: mm_release ... had_active=0`。

### 3. 停止点

raw 输出文件：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/kernel-disable-bump-prefault-full-pow2-kib-4-18-thread-touch-both-3rep-20260520-203343/raw/threads02_rep01.txt
```

最后阶段停在 traditional path：

```text
bench_phase: path=traditional (cache/mmap) delegated=0 size=16777216 phase=before_touch kind=warmup iter=10 cpu=0 ptr=0x7623fff000
bench_phase: path=traditional (cache/mmap) delegated=0 size=16777216 phase=after_touch kind=warmup iter=9 cpu=1 ptr=0x7621dff000
```

即：

```text
threads = 2
path = traditional (cache/mmap)
size = 16MiB
phase = warmup touch
```

### 4. 当前判断

这次 full both-path 失败和前一节的 bump-prefault 根因不是同一个直接触发点：

- bump-prefault 修复后，`threads=2, 16KiB..256MiB, delegated, touch` 已完整通过。
- full both-path 仍会在 `threads=2` 的 traditional mmap touch 阶段触发重启。

补充确认 benchmark 代码路径：

```text
--path traditional:
  setenv("SCUDO_SHARED_ARENA_DISABLE", "1")
  skip Pool.init()
  SharedArena: ready=0

--path both:
  Pool.init()
  SharedArena: ready=1
  traditional phase only calls setSharedArenaForceForTesting(false)
```

也就是说，`--path both` 里的 traditional 不是“完全未开启 delegated”的传统路径；
它只是 allocator route force-off，但进程已经注册/attach 了 SharedArena，内核侧
active mm 状态仍然存在。

因此当前状态应拆成两层：

1. **已定位并验证的直接 bug**：
   bump allocation 不应携带 `kLogFlagPrefault`，否则 delegated touched 序列会
   在内核 PTE sync/task_work 中整段同步 prefault，导致重启。
2. **仍需定位的组合 bug**：
   traditional 自身在未开启 delegated 的情况下已经验证稳定；`--path both`
   在每个 size 内的执行顺序是 traditional 先跑、delegated 后跑。因此这次停在
   `16MiB traditional`，更准确地说是前序较小 size 的 delegated 状态污染了后续
   size 的 ordinary traditional mmap/touch。后续 traditional 阶段本身不从
   SharedArena 分配，但普通缺页、调度和返回用户态仍可能触发 active mm 相关的
   task_work/PTE sync 或残留 log/state 处理路径，最终在
   `threads=2,size=16MiB` warmup touch 附近重启。

下一步建议：

- 跑 `--path both` 但缩小 sizes 到 `16KiB..16MiB`、`threads=2`，确认最小复现前缀。
- 对照跑 `--path both --sizes 16777216`，如果单 size 稳定，则进一步证明触发条件是
  “前序 delegated 状态 + 后续 traditional touch”。
- 在内核侧给 `md_memory_delegation_sync_mm()`、task_work drain、active mm lookup、
  log apply/drop 路径增加限频 trace，记录 traditional 阶段是否仍在处理前序 arena 状态。

### 5. 单点对照：traditional-only vs both

进一步跑 `threads=2,size=16MiB,touch` 单点对照：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16777216 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path both" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/investigate-both-single-16m-thread2-touch-20260520
```

结果：设备重启。停止点仍在 traditional warmup，且尚未进入 delegated phase：

```text
SharedArena: ready=1 num_cores=8
arena_debug: label=before_traditional ready=1 num_cores=8
bench_phase: path=traditional (cache/mmap) delegated=0 size=16777216 phase=before_touch kind=warmup iter=1 cpu=0
```

恢复后：

```text
ro.boot.bootreason = reboot
/proc/uptime = 46.66 342.39
/sys/fs/pstore = empty
```

对照运行：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16777216 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path traditional" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/control-traditional-single-16m-thread2-touch-20260520
```

结果：完整通过，`parsed_records=4`。raw 开头确认：

```text
SharedArena: ready=0 num_cores=0
arena_debug: label=before_traditional ready=0 num_cores=0
```

这个对照把触发条件进一步收窄为：

```text
不是 traditional mmap/touch 自身不稳定；
而是进程只要 Pool.init()/register 了 SharedArena active mm，
即使 allocator route 已 force-off，普通 16MiB mmap touch 也可能触发
memory delegation 的 active-mm/task_work/PTE-sync 路径并导致 reboot。
```

下一步根因方向应从“前序 delegated log 污染”再向前推进到：

- `md_register_mm()` 后 active mm 是否会让所有普通 fault/return-to-user 都进入 sync。
- active mm 中没有 pending log、没有 arena allocation 时，sync 是否仍错误扫描/操作
  ordinary VMA 或 arena chunk state。
- force-off traditional phase 是否需要用户态临时 unregister/suspend active mm，
  或内核侧 sync 需要在无 pending log/无 delegated fault 时快速返回。

## 2026-05-20 generic page fault hook 根因确认

### 1. 代码根因

继续查内核侧后，发现当前实现仍在 `mm/memory.c::handle_pte_fault()` 的
generic page fault 路径中调用：

```c
if (!vmf->pte &&
    unlikely(!memory_delegation_fault_allowed(vmf->vma->vm_mm,
                                             vmf->address,
                                             vmf->vma)))
        return VM_FAULT_SIGSEGV;
```

这和 `DESIGN.md §2.6` 的安全红线直接冲突：

```text
绝对不修改 Linux 核心的 generic page fault 锁与匿名页缺页路径；
把所有权校验与补页决策下沉到专用 VMA 的自定义 fault 慢路径中。
```

`--path traditional` 单独运行时会设置 `SCUDO_SHARED_ARENA_DISABLE=1` 并跳过
`Pool.init()`，所以 `SharedArena: ready=0`，generic hook 快速返回。

`--path both` 则会先 `Pool.init()` 并通过 `prctl()` 注册 active mm，即使
traditional phase 调用了 `setSharedArenaForceForTesting(false)`，普通 mmap/touch
产生的每个匿名缺页仍会进入 generic memory delegation hook。这个 hook 会在
缺页路径中遍历 active ctx / arena metadata，破坏了“普通 VMA 完全不受影响”的
设计边界；在 `threads=2,size=16MiB,touch` 这种大量并发缺页场景下，最终表现为
设备 reboot/watchdog 级故障。

### 2. 修复

移除 generic page fault hook，只保留已经安装在 arena VMA 上的
`md_arena_vm_ops->fault` / `md_arena_vma_page_mkwrite`：

```text
mm/memory.c:
  删除 #include <linux/memory_delegation.h>
  删除 handle_pte_fault() 中的 memory_delegation_fault_allowed() 调用

include/linux/memory_delegation.h:
  删除 memory_delegation_fault_allowed() 声明和 !CONFIG stub

mm/memory_delegation.c:
  删除 memory_delegation_fault_allowed() 实现和 EXPORT_SYMBOL_GPL
```

arena VMA 的所有权校验仍由 `md_arena_vm_ops->fault` 调用
`md_fault_addr_allowed()` 完成；普通匿名/file mmap 不再进入 memory delegation
检查。

### 3. 验证

内核构建：

```bash
cd /home/lrc/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
```

结果：构建成功。

刷机：

```bash
/home/lrc/patent/flush_kernel.sh
```

#### 最小复现验证

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16777216 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path both" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/verify-remove-generic-fault-hook-both-single-16m-thread2-touch-20260520
```

结果：通过。

```text
[done] repetition=1/1 threads=2 parsed_records=8
```

这组参数在修复前稳定导致设备重启。

#### threads=2 full sizes 验证

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/verify-remove-generic-fault-hook-both-full-thread2-touch-1rep-20260520
```

结果：通过。

```text
[done] repetition=1/1 threads=2 parsed_records=120
```

#### full both-path 单轮验证

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/verify-remove-generic-fault-hook-both-full-pow2-thread-touch-1rep-20260520
```

结果：`threads=1,2,4,8` 全部通过。

```text
[done] repetition=1/1 threads=1 parsed_records=120
[done] repetition=1/1 threads=2 parsed_records=120
[done] repetition=1/1 threads=4 parsed_records=120
[done] repetition=1/1 threads=8 parsed_records=120
```

运行后设备 uptime 持续递增，无 reboot；`dmesg` 未出现新的 panic/oops/watchdog
崩溃栈。

### 4. 当前结论

这轮 `--path both` 在 traditional 阶段重启的直接根因是：

```text
memory delegation 仍挂在 generic page fault 路径上。
当进程 register active mm 后，ordinary traditional mmap/touch 的匿名缺页
会进入 memory_delegation_fault_allowed()，
违反 DESIGN.md “只在专用 arena VMA fault 慢路径检查所有权”的约束，
并在多线程大块 touched workload 下触发 reboot/watchdog 级故障。
```

正式修复应保留：

- 不再从 `mm/memory.c` 的 generic fault 路径调用 memory delegation。
- 所有 arena 所有权检查只发生在 `md_arena_vm_ops->fault/page_mkwrite`。
- `memory_delegation_fault_allowed()` 可以删除，避免后续再次误接入普通 fault path。

## 2026-05-20 收敛调试日志后的 3rep 稳定性验证

### 1. 日志收敛

删除前面为定位 reboot 加入的高频正常路径日志：

- `ctx free` 生命周期日志。
- `register start/done` 日志。
- `queue sync`、`sync task start/done` 日志。
- `mm_release before/after drain` per-cpu ring snapshot。
- `mm_release ctx remove` 逐 ctx 日志。

保留：

- `pr_warn_ratelimited()` 异常路径。
- `md_log_fault_deny_rcu()` 的限频拒绝日志，用于后续定位真实越权访问。
- `mm_release done` 的限频汇总日志。

内核重新构建并刷机：

```bash
cd /home/lrc/patent/kernel/kernel_platform
tools/bazel run //common:kernel_aarch64_dist -- --destdir=./out
/home/lrc/patent/flush_kernel.sh
```

结果：构建和刷机成功。刷机后设备恢复：

```text
/proc/uptime = 46.27 339.80
kernel = Linux localhost 6.12.23-android16-5-o-g576abba954b4-4k
```

### 2. smoke test

先跑修复前稳定重启的最小用例：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 2 \
  --sizes 16777216 \
  --iterations 80 --warmup 20 --repetitions 1 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path both" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/smoke-clean-logs-both-single-16m-thread2-touch-20260520
```

结果：

```text
[done] repetition=1/1 threads=2 parsed_records=8
```

### 3. full benchmark 3rep

运行配置：

```bash
python3 /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py \
  --remote-host lrc@192.168.61.230 \
  --skip-build --skip-deploy --use-su \
  --thread-counts 1,2,4,8 \
  --sizes 16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216,33554432,67108864,134217728,268435456 \
  --iterations 80 --warmup 20 --repetitions 3 \
  --bench-extra-args "--allocator combined --mode thread --touch-pages 1 --path both" \
  --results-dir /home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/verify-clean-logs-remove-generic-fault-hook-both-full-pow2-thread-touch-3rep-20260520
```

结果目录：

```text
/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/microbench-results/verify-clean-logs-remove-generic-fault-hook-both-full-pow2-thread-touch-3rep-20260520
```

12 个 run 全部完成：

```text
rep1 threads=1 parsed_records=120
rep1 threads=2 parsed_records=120
rep1 threads=4 parsed_records=120
rep1 threads=8 parsed_records=120
rep2 threads=1 parsed_records=120
rep2 threads=2 parsed_records=120
rep2 threads=4 parsed_records=120
rep2 threads=8 parsed_records=120
rep3 threads=1 parsed_records=120
rep3 threads=2 parsed_records=120
rep3 threads=4 parsed_records=120
rep3 threads=8 parsed_records=120
```

输出数据：

```text
raw_results.csv = 1441 lines  # header + 1440 records
summary.csv = 481 lines       # header + 480 aggregated rows
```

运行后设备状态：

```text
/proc/uptime = 302.63 1875.30
ro.boot.bootreason = bootloader
```

`dmesg` 未出现新的 `panic/oops/BUG/reboot` 或
`memory_delegation: deny arena access`。仍可看到少量限频
`memory_delegation: mm_release done ... released_pages=0` 汇总日志，以及系统自身
`OPLUS_WD`/hung-task 监控信息；benchmark 过程中设备未掉线、未重启。

### 4. 稳定性结论

在清理临时日志后：

- 最小复现 `threads=2,size=16MiB,path=both,touch=1` 通过。
- full both-path 3rep 全部通过。
- traditional baseline 与 delegated path 均被纳入统计。
- 未复现此前 reboot/watchdog 故障。

当前可以认为：

```text
generic page fault hook 移除后，previous both-path reboot 已被修复；
bump allocation prefault 移除后，delegated touched full-size 路径也保持稳定。
```

后续若要进一步给最终性能数据背书，可在同一内核上追加 5rep 或更长时间 soak；
但截至本次 3rep，稳定性验证已经覆盖此前所有已知触发点。
