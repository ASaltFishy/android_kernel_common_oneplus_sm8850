# SQLite 真实应用 workload 测试结果

本文档记录 memory delegation / Scudo SharedArena 在 SQLite 真实数据库 workload
上的测试方法、workload 内容和真机测试结果。

## 1. 测试目的

SQLite 测试用于验证 SharedArena 在真实 native/C 数据库应用中的效果：

- 性能：对比 baseline Scudo 与 SharedArena 路径下，SQLite workload 的端到端耗时。
- 内存：对比 workload 完成后系统 `MemAvailable` 下降量、进程组 PSS/private。
- 真实性：数据库引擎使用 Android 系统真实 `libsqlite.so`，测试程序只负责调度 workload 和统计。

需要注意的是，SQLite 的 live page cache、TEMP table、索引构建中仍被应用持有的数据不会因为 SharedArena 减少；SharedArena 主要优化的是 Scudo Secondary 大块分配/free 后的 allocator cache 复用。因此 SQLite 结果预期不会像专门的 cache wave benchmark 那样大幅降低 PSS。

## 2. 测试程序

测试程序路径：

```text
tools/testing/memory_delegation/tests/src/shared_arena_sqlite_app.cpp
```

构建输出：

```text
tools/testing/memory_delegation/tests/out/shared_arena_sqlite_app
```

测试程序运行时通过 `dlopen("libsqlite.so")` 加载 Android 系统 SQLite，并通过
`dlsym()` 调用真实 SQLite API。

在每个 worker 进程中，程序会在 `sqlite3_initialize()` 前调用：

```text
sqlite3_config(SQLITE_CONFIG_MALLOC, ...)
```

将 SQLite 内部 allocator 配置为当前进程的 `malloc/free/realloc`。由于测试二进制静态链接了修改版 Scudo wrapper，SQLite 内部内存分配会进入本项目的 Scudo 路径。baseline 与 SharedArena 仅通过环境变量切换。

## 3. Workload 内容

测试使用 4 个 worker 进程，每个 worker 独立打开自己的 SQLite 数据库文件。worker 之间不共享数据库文件，避免 SQLite 文件锁成为主要干扰。

单个 worker 的操作：

1. 打开独立数据库文件。
2. 设置 SQLite pragma：
   - `journal_mode=OFF`
   - `synchronous=OFF`
   - `temp_store=MEMORY`
   - `cache_size=-32768`
3. 创建表：

```sql
CREATE TABLE kv(k INTEGER PRIMARY KEY, worker INTEGER, payload TEXT);
```

4. 使用 prepared statement 插入 `10000` 行数据，每行 payload 为 `2048` 字节。
5. 执行 2 轮查询/临时表 workload：
   - 创建索引：`CREATE INDEX IF NOT EXISTS idx_payload ON kv(payload)`
   - 内存临时表排序：

```sql
CREATE TEMP TABLE tmp_sort AS
SELECT payload FROM kv ORDER BY payload DESC;
```

   - 删除临时表。
   - 执行 range query：

```sql
SELECT count(*), sum(length(payload)) FROM kv WHERE payload >= ?;
```

6. worker 完成后关闭数据库，但保持进程存活，便于父进程读取 `/proc/<pid>/smaps_rollup` 统计内存。

本轮测试参数：

| 参数 | 值 |
| --- | ---: |
| worker 数量 | 4 |
| 每 worker 行数 | 10000 |
| payload 大小 | 2048 B |
| query/sort rounds | 2 |
| 执行模式 | sequential |
| CPU 绑定 | CPU 0 |

## 4. 统计指标

测试程序输出以下关键行：

```text
SQLITE_MEM phase=...
SQLITE_WORKER id=...
SQLITE_RESULT workers=...
PASS(sqlite): shared arena sqlite workload ok
```

主要指标：

| 指标 | 含义 |
| --- | --- |
| `elapsed_ns` | 4 个 worker 顺序完成 workload 的端到端耗时 |
| `mem_drop_kb` | 测试开始到最终采样时，系统 `MemAvailable` 减少量 |
| `final_group_pss_kb` | 父进程 + worker 进程组最终 PSS |
| `final_group_private_kb` | 父进程 + worker 进程组最终 private 内存 |
| `final_scudo_secondary_pss_kb` | 映射名包含 `scudo:secondary` 的 Scudo Secondary 私有映射 PSS |
| `final_scudo_secondary_private_kb` | `scudo:secondary` 映射的 private 内存 |
| `final_arena_pss_kb` | SharedArena 固定 VA 区间内映射的 PSS |
| `final_arena_private_kb` | SharedArena 固定 VA 区间内映射的 private 内存 |
| `final_arena_shared_kb` | SharedArena 固定 VA 区间内映射的 shared 内存 |

`MemAvailable drop` 反映系统整体可用内存少了多少；PSS/private 更偏向这组测试进程自身的占用。

allocator 相关指标来自解析每个测试进程的 `/proc/<pid>/smaps`：

- `scudo_secondary_*`：映射名包含 `scudo:secondary`，表示 baseline Scudo Secondary 私有映射。
- `arena_*`：地址落在 SharedArena 固定 VA 区间，表示 SharedArena 实际驻留内存。

## 5. 构建命令

在内核 common 目录执行：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

make -C tools/testing/memory_delegation/tests MODE=android \
  shared_arena_sqlite_app memory_delegation_broker
```

## 6. 运行命令

### 6.1 Baseline

baseline 关闭强制 SharedArena 路径：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=0 USE_SU=1 WAIT_SECS=300 \
LOCAL_BIN=tools/testing/memory_delegation/tests/out/shared_arena_sqlite_app \
EXTRA_ENV="SCUDO_SHARED_ARENA_FORCE=0 SCUDO_SHARED_ARENA_TRACE=0 SHARED_ARENA_SQLITE_WORKERS=4 SHARED_ARENA_SQLITE_ROWS=10000 SHARED_ARENA_SQLITE_ROUNDS=2 SHARED_ARENA_SQLITE_PAYLOAD_BYTES=2048 SHARED_ARENA_SQLITE_MODE=sequential SHARED_ARENA_SQLITE_HOLD_SECS=1" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

### 6.2 SharedArena

SharedArena 使用默认 runner 注入的 `SCUDO_SHARED_ARENA_FORCE=1`：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=0 USE_SU=1 WAIT_SECS=300 \
LOCAL_BIN=tools/testing/memory_delegation/tests/out/shared_arena_sqlite_app \
EXTRA_ENV="SCUDO_SHARED_ARENA_TRACE=0 SHARED_ARENA_SQLITE_WORKERS=4 SHARED_ARENA_SQLITE_ROWS=10000 SHARED_ARENA_SQLITE_ROUNDS=2 SHARED_ARENA_SQLITE_PAYLOAD_BYTES=2048 SHARED_ARENA_SQLITE_MODE=sequential SHARED_ARENA_SQLITE_HOLD_SECS=1" \
tools/testing/memory_delegation/scripts/run_phone_over_ssh_adb.sh
```

## 7. 真机测试结果

测试日志：

```text
baseline:
/tmp/md-phone-run/phone-sqlite-alloc-baseline-20260521-162753.out.txt

SharedArena:
/tmp/md-phone-run/phone-sqlite-alloc-shared-20260521-162820.out.txt
```

结果表：

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| 端到端耗时 | 10.065 s | 9.388 s | 1.07x 加速 |
| `MemAvailable` drop | 535.4 MiB | 528.7 MiB | 减少 6.7 MiB，降低 1.3% |
| 最终进程组 PSS | 530.5 MiB | 530.4 MiB | 基本不变 |
| 最终进程组 private | 513.2 MiB | 512.9 MiB | 基本不变 |
| Scudo Secondary PSS | 0.52 MiB | 0 MiB | baseline 私有 Secondary cache 很小 |
| Scudo Secondary private | 0.52 MiB | 0 MiB | baseline 私有 Secondary cache 很小 |
| SharedArena PSS | 0 MiB | 0.13 MiB | arena 驻留很小 |
| SharedArena private | 0 MiB | 0 MiB | arena private 基本为 0 |
| SharedArena shared | 0.16 MiB | 0.68 MiB | 共享映射/元数据 |

原始输出：

```text
baseline:
SQLITE_RESULT workers=4 rows=10000 rounds=2 elapsed_ns=10065288798 mem_drop_kb=548252 final_group_pss_kb=543255 final_group_private_kb=525500 final_scudo_secondary_pss_kb=528 final_scudo_secondary_private_kb=528 final_arena_pss_kb=0 final_arena_private_kb=0 final_arena_shared_kb=160

SharedArena:
SQLITE_RESULT workers=4 rows=10000 rounds=2 elapsed_ns=9388251402 mem_drop_kb=541344 final_group_pss_kb=543147 final_group_private_kb=525220 final_scudo_secondary_pss_kb=0 final_scudo_secondary_private_kb=0 final_arena_pss_kb=133 final_arena_private_kb=0 final_arena_shared_kb=692
```

异常检查：

```text
stdout/dmesg/logcat 未发现：
FAIL
Kernel panic
Oops
BUG:
Scudo ERROR
CHECK
```

logcat 中仅出现 Android power stats 的 `EX_TRANSACTION_FAILED` 噪声，与测试无关。

## 8. 结论

SQLite 真实应用 workload 中，SharedArena 带来了小幅端到端性能收益：

```text
10.065 s -> 9.388 s，约 1.07x
```

系统可用内存下降量减少约：

```text
6.7 MiB，约 1.3%
```

最终进程组 PSS/private 基本不变。真实 SQLite workload 内的 allocator cache 统计也显示：baseline 中 `scudo:secondary` 私有映射仅约 0.52 MiB，SharedArena 中 arena PSS 仅约 0.13 MiB。也就是说，本 SQLite workload 的 500 MiB 级内存占用几乎不来自 Scudo Secondary 私有 cache，而主要来自 SQLite live working set，例如 page cache、TEMP table、索引/排序期间仍被应用持有的数据。SharedArena 不会减少仍被应用持有的 live 数据，因此真实 SQLite 场景中的内存节省较小。

该结果适合作为真实应用补充实验：证明 SharedArena 在数据库场景下不会破坏正确性，并能带来小幅性能和系统可用内存改善；同时也说明 SQLite 不是展示 allocator cache 合并收益的强场景。
