# Mixed media native flow 测试记录

本文档记录一次面向 SharedArena 的手机端多进程混合 workload 试跑。

## 1. 目标

验证“多个真实 native 媒体/压缩/索引任务在手机上并发或交错运行”是否比 Ollama
更适合当前 SharedArena 模式。

本轮不是微基准循环 malloc/free，而是复用现有真实库/接口 workload：

- image pipeline：模拟图片 decode -> resize -> filter -> encode 的 native 多阶段处理。
- zstd：调用真实 zstd 压缩/解压 API。
- sqlite：通过 `dlopen("libsqlite.so")` 调用 Android 系统 SQLite API。

三者作为独立顶层进程同时运行，内部再 fork worker/stage 子进程。

## 2. 脚本

新增脚本：

```text
tools/testing/memory_delegation/scripts/run_phone_mixed_media_flow.sh
```

该脚本会：

1. 构建或复用 Android 版 workload 二进制。
2. 上传到 macOS 跳板机。
3. 通过 adb push 到手机 `/data/local/tmp/md`。
4. 启动 `memory_delegation_broker`。
5. 分别运行 baseline 与 SharedArena 两组。
6. 收集各 workload 自身输出的内存和耗时统计。

运行命令：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

BUILD=0 RUN_BASELINE=1 RUN_SHARED=1 \
tools/testing/memory_delegation/scripts/run_phone_mixed_media_flow.sh
```

本轮参数：

```text
image:  1920x1080, 12 images, warmup=1
zstd:   4 workers, 12 chunks/worker, 1 MiB/chunk, concurrent
sqlite: 4 workers, 5000 rows/worker, 1024 B payload, sequential
```

## 3. 日志

有效复测日志：

```text
/tmp/md-phone-run/mixed-media-flow-20260528-143529.txt
```

注：脚本中的外层 sampler 第一版只采到一个 sample，原因是 shell/`set -u`
对空统计处理不够稳健。该问题已在脚本中修复；本轮结论主要依据各 workload
内部已经稳定输出的 `*_RESULT` / `*_MEM` 行。

## 4. 结果

### 4.1 image pipeline

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| elapsed | 4.301 s | 4.036 s | SharedArena 快约 6.2% |
| stage total | 4.205 s | 3.829 s | SharedArena 快约 8.9% |
| total alloc bytes | 343.3 MiB | 343.3 MiB | 相同 |
| internal peak PSS | 29.1 MiB | 34.6 MiB | SharedArena 高约 5.5 MiB |

本项显示 SharedArena 路径可跑通，且在该轮没有性能退化。但该 workload 是
自研简化 image pipeline，不应单独作为“真实应用收益”的主证据。

### 4.2 zstd concurrent

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| elapsed | 13.131 s | 13.187 s | 基本持平，SharedArena 慢约 0.4% |
| peak group PSS | 66.9 MiB | 67.0 MiB | 基本不变 |
| peak allocator PSS | 53.2 MiB secondary | 53.2 MiB arena | 承载位置迁移 |
| final group PSS | 101.1 MiB | 114.8 MiB | SharedArena 高约 13.7 MiB |
| final private | 86.9 MiB | 100.2 MiB | SharedArena 高约 13.3 MiB |
| final secondary PSS | 85.8 MiB | 0 MiB | 私有 cache 消除 |
| final arena PSS | 0 MiB | 99.4 MiB | 共享 freelist 保留 |
| secondary cache bytes | 132.1 MiB | 0 MiB | 私有 cache 消除 |
| arena free reusable bytes | 0 MiB | 173.4 MiB | 释放后进入共享 freelist |

该结果和已有 zstd concurrent 结论一致：单轮并发 first wave 中，SharedArena 不减少
live peak；释放后虽然消除了 per-process private Secondary cache，但 arena 会保留整波
live set 作为共享可复用 freelist。如果后续没有第二波任务复用这些块，最终 resident
footprint 不一定更低。

### 4.3 sqlite

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| elapsed | 16.472 s | 16.441 s | 基本持平 |
| final group PSS | 211.6 MiB | 212.3 MiB | 基本不变 |
| final secondary PSS | 0 MiB | 0 MiB | baseline 本身无明显 Secondary cache |
| final arena PSS | 0 MiB | 0.64 MiB | 很小 |

SQLite 在本参数下仍不是强收益场景，主要内存来自 live page cache / TEMP / 查询期间
仍被应用持有的数据，而非 Scudo Secondary 私有 cache。

## 5. 结论

本轮 mixed media flow 证明：

- 多进程组合 workload 可以在手机上稳定运行。
- SharedArena attach / broker / 多进程 arena 映射链路正常。
- zstd 部分明确发生了 Secondary -> SharedArena 的承载迁移：

```text
final_scudo_secondary_pss_kb: 85824 -> 0
final_arena_pss_kb:          0 -> 99428
final_arena_free_reusable:   0 -> 173441024 bytes
```

但本轮单次并发 first wave **没有测出总体内存节省**。原因不是 SharedArena 没生效，
而是 workload 形态仍然偏“同时持有 live buffer 的一波任务”。SharedArena 的收益要靠
后续任务复用已经进入共享 freelist 的块才能体现。

## 6. 下一步

下一版 mixed flow 应改成自然的多轮任务流，而不是单轮并发 first wave：

1. 第一波：图片处理 + zstd + sqlite 同时运行，制造真实 allocator cache。
2. 间隔短暂停顿，进程不强制按 allocator 顺序调度，但任务自然结束并释放 buffer。
3. 第二波：再次运行图片处理 / zstd / sqlite，让新进程复用上一波释放到 arena 的块。
4. 比较第二波的 `MemAvailable` drop、mmap/secondary PSS、arena reusable/in-use、
   吞吐和端到端耗时。

如果目标是证明 SharedArena 的正向收益，双波或持续循环的“相册批处理 + 压缩发布”
比单轮并发更符合当前 arena 设计。
