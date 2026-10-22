# Ollama 端侧 LLM workload 测试记录

本文档记录在手机端使用 Ollama 作为真实 LLM 推理 workload，验证
memory delegation / Scudo SharedArena 是否能够减少进程私有 allocator cache
带来的内存占用。测试日期：2026-05-26。

## 1. 测试目标

- 确认当前 flash 刷入的 memory delegation 内核在手机上可用。
- 确认 Android broker / SharedArena 基础链路可用。
- 探测手机上 Termux / Ollama 的安装位置、可用模型和运行方式。
- 对比 baseline 与 SharedArena 下 Ollama workload 的内存表现。

注意：SharedArena 预期优化的是进程释放后的 Scudo Secondary cache 从
“每进程私有缓存”变为“跨进程共享可复用 freelist”。模型权重 mmap、正在使用的
KV cache 或 Go runtime live heap 不应被计入可节省对象。

## 2. 环境与固定命令

本地内核 common 目录：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common
```

跳板机与 adb：

```bash
JUMP_HOST=lrc@192.168.61.230
ADB_BIN=/opt/homebrew/bin/adb
ADB_SERIAL=3B15AL00K5D00000
PHONE_DIR=/data/local/tmp/md
```

手机状态约束：

- 当前手机已经通过 `~/patent/flush_kernel.sh --mode flash` 刷入改过的内核。
- 当前刷入内核下手机无 Wi-Fi 和蓝牙，因此测试依赖 USB adb + macOS 跳板机。
- 手机上已有 Termux、Ollama 服务和若干 Ollama 模型。

## 3. 阶段 0：ADB / root / debugfs 连通性

执行时间：2026-05-26 15:59 CST。

### 3.1 检查 adb 设备

命令：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb devices -l"
```

结果摘要：

```text
3B15AL00K5D00000 device usb:1-1 product:PLK110 model:PLK110 device:OP60FFL1
```

## 5. 阶段 2：Termux / Ollama 探测

执行时间：2026-05-26 16:06-16:18 CST。

### 5.1 探测 Termux / Ollama 位置

命令：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'ls -l /data/data/com.termux/files/usr/bin/ollama 2>/dev/null || true; \
    ls -ld /data/data/com.termux /data/data/com.termux/files /data/data/com.termux/files/home 2>/dev/null; \
    stat -c \\\"%u %g %U %G %a %n\\\" /data/data/com.termux/files/home 2>/dev/null || true'\""
```

结果摘要：

```text
/data/data/com.termux/files/usr/bin/ollama
owner: u0_a377:u0_a377
Termux home: /data/data/com.termux/files/home
```

确认二进制格式：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'file /data/data/com.termux/files/usr/bin/ollama 2>/dev/null || \
    toybox file /data/data/com.termux/files/usr/bin/ollama 2>/dev/null || \
    ls -lh /data/data/com.termux/files/usr/bin/ollama'\""
```

结果摘要：

```text
ELF shared object, 64-bit LSB arm64, dynamic (/system/bin/linker64), for Android 24
```

### 5.2 探测本地模型

启动 baseline server 后再次 list：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'cd /data/local/tmp/md && \
    for p in \$(pidof ollama 2>/dev/null); do kill -TERM \$p || true; done; \
    sleep 1; \
    for p in \$(pidof ollama 2>/dev/null); do kill -KILL \$p || true; done; \
    rm -f ollama_probe_baseline.log ollama_probe_baseline.pid; \
    HOME=/data/data/com.termux/files/home \
    PREFIX=/data/data/com.termux/files/usr \
    PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin \
    LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib \
    OLLAMA_HOST=127.0.0.1:11434 \
    OLLAMA_MODELS=/data/data/com.termux/files/home/.ollama/models \
    SCUDO_SHARED_ARENA_FORCE=0 \
    SCUDO_SHARED_ARENA_TRACE=0 \
    /data/data/com.termux/files/usr/bin/ollama serve > /data/local/tmp/md/ollama_probe_baseline.log 2>&1 & \
    echo \$! > /data/local/tmp/md/ollama_probe_baseline.pid'\""

ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'HOME=/data/data/com.termux/files/home \
    PREFIX=/data/data/com.termux/files/usr \
    PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin \
    LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib \
    OLLAMA_HOST=127.0.0.1:11434 \
    OLLAMA_MODELS=/data/data/com.termux/files/home/.ollama/models \
    /data/data/com.termux/files/usr/bin/ollama list 2>&1'\""
```

结果摘要：

```text
deepseek-r1:1.5b    1.1 GB
deepseek-r1:7b      4.7 GB
```

### 5.3 baseline 生成探针

命令：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'cd /data/local/tmp/md && \
    HOME=/data/data/com.termux/files/home \
    PREFIX=/data/data/com.termux/files/usr \
    PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin \
    LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib \
    /data/data/com.termux/files/usr/bin/curl -sS --max-time 240 \
      -X POST http://127.0.0.1:11434/api/generate \
      -d '\\''{\\\"model\\\":\\\"deepseek-r1:1.5b\\\",\\\"prompt\\\":\\\"用一句话回答：1+1等于几？\\\",\\\"stream\\\":false,\\\"options\\\":{\\\"num_predict\\\":32,\\\"num_ctx\\\":512}}'\\'' \
      > ollama_probe_generate.json; \
    rc=\$?; echo rc=\$rc; head -c 500 ollama_probe_generate.json; echo'\""
```

结果摘要：

```text
rc=0
POST /api/generate completed
```

baseline runner 采样命令：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'ps -A -o USER,PID,PPID,NAME,ARGS | \
    grep -E \\\"ollama|memory_delegation\\\" | grep -v grep; \
    for p in \$(pidof ollama 2>/dev/null); do \
      echo === PID \$p CMD ===; tr \\\"\\\\0\\\" \\\" \\\" < /proc/\$p/cmdline; echo; \
      cat /proc/\$p/smaps_rollup 2>/dev/null | grep -E \\\"Pss:|Private_|Shared_|Rss:|Swap:\\\"; \
      grep -E \\\"scudo:secondary|memory_delegation|shared_arena|arena|libc_malloc|ollama/models|blob|gguf\\\" \
        /proc/\$p/smaps 2>/dev/null | head -n 220; \
    done'\""
```

结果摘要：

```text
ollama serve PSS: ~27.9 MiB
ollama runner PSS: ~1117.1 MiB
ollama runner Private_Dirty: ~1104.8 MiB
runner maps: large [anon:libc_malloc]
runner log: load_tensors mmap=false, CPU model buffer size = 1059.89 MiB
```

结论：Ollama 1.5B workload 可运行，并且模型权重走 `mmap=false` 的大块匿名内存，
不是文件 mmap；这本来是适合验证 allocator 路径的 workload。

### 5.5 尝试构建 Android LD_PRELOAD Scudo

为验证是否能在不重编 Ollama 的情况下拦截 malloc/free，手工编译了
aarch64 Android 版 `libscudo_md_android.so`：

修复后已在 Makefile 中固化 target：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

make -B -C tools/testing/memory_delegation/tests MODE=android libscudo_md_android_so
```

关键编译要求：

```text
-shared -fPIC
-fno-emulated-tls
-DSCUDO_SHARED_ARENA_BASE_ADDR=0x1000000000ULL
```


### 5.6 Ollama 通过 LD_PRELOAD 接入修改版 Scudo

第一次把 `LD_PRELOAD` 接到 Ollama 时，如果保留 Termux 的
`LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib`，linker 报错：

```text
CANNOT LINK EXECUTABLE "/data/data/com.termux/files/usr/bin/ollama":
cannot locate symbol "Xzs_Construct" referenced by "/system/lib64/libunwindstack.so"
```

原因：系统 `libunwindstack.so` 依赖系统 `liblzma.so` 的私有符号
`Xzs_Construct`；Termux `LD_LIBRARY_PATH` 把 Termux 的 `liblzma.so` 放到了系统库
之前，导致符号解析失败。解决方式是启动 Ollama 时不要设置 Termux
`LD_LIBRARY_PATH`；该 Ollama 二进制自身 RUNPATH 已包含 Termux lib 路径，仍可找到
`libc++_shared.so` 等依赖。

成功启动命令：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'cd /data/local/tmp/md && \
    for p in \$(pidof ollama 2>/dev/null); do kill -TERM \$p || true; done; \
    sleep 1; \
    for p in \$(pidof ollama 2>/dev/null); do kill -KILL \$p || true; done; \
    for p in \$(pidof memory_delegation_broker 2>/dev/null); do kill -TERM \$p || true; done; \
    sleep 1; \
    for p in \$(pidof memory_delegation_broker 2>/dev/null); do kill -KILL \$p || true; done; \
    rm -f broker.log ollama_preload_noldpath.log; \
    ./memory_delegation_broker --trace >broker.log 2>&1 & \
    for i in \$(seq 1 100); do grep -q \\\"ready num_cores=\\\" broker.log && break; sleep 0.1; done; \
    HOME=/data/data/com.termux/files/home \
    PREFIX=/data/data/com.termux/files/usr \
    PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin \
    OLLAMA_HOST=127.0.0.1:11434 \
    OLLAMA_MODELS=/data/data/com.termux/files/home/.ollama/models \
    OLLAMA_KEEP_ALIVE=-1 \
    SCUDO_SHARED_ARENA_FORCE=1 \
    SCUDO_SHARED_ARENA_TRACE=1 \
    LD_PRELOAD=/data/local/tmp/md/libscudo_md_android.so \
    /data/data/com.termux/files/usr/bin/ollama serve > /data/local/tmp/md/ollama_preload_noldpath.log 2>&1 & \
    echo \$! > /data/local/tmp/md/ollama_preload_noldpath.pid; \
    sleep 3; \
    head -n 80 ollama_preload_noldpath.log; \
    ps -A -o USER,PID,PPID,NAME,ARGS | grep -E \\\"ollama|memory_delegation\\\" | grep -v grep'\""
```

启动日志关键证据：

```text
[scudo][shared_arena] request broker fd core=0 success fd=5
[scudo][shared_arena] android init mode=attach (system broker ready)
[scudo][shared_arena] arena init core=0 success base=0x1000000000 fd=5
[scudo][shared_arena] arena init core=1 success base=0x1020000000 fd=6
...
[scudo][shared_arena] arena init core=7 success base=0x10e0000000 fd=12
[scudo][shared_arena] pool init finished ready=1
```

执行推理：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb shell \"su -c 'cd /data/local/tmp/md && \
    HOME=/data/data/com.termux/files/home \
    PREFIX=/data/data/com.termux/files/usr \
    PATH=/data/data/com.termux/files/usr/bin:/system/bin:/system/xbin \
    /data/data/com.termux/files/usr/bin/curl -sS --max-time 300 \
      -X POST http://127.0.0.1:11434/api/generate \
      -d '\\''{\\\"model\\\":\\\"deepseek-r1:1.5b\\\",\\\"prompt\\\":\\\"用一句话回答：1+1等于几？\\\",\\\"stream\\\":false,\\\"keep_alive\\\":-1,\\\"options\\\":{\\\"num_predict\\\":32,\\\"num_ctx\\\":512}}'\\'' \
      > ollama_preload_generate.json; \
    rc=\$?; echo rc=\$rc; head -c 500 ollama_preload_generate.json; echo'\""
```

结果：

```text
rc=0
POST /api/generate completed
```

采样证据：

```text
ollama serve pid=31518
ollama runner pid=31572
runner cmd: ollama runner --model ...deepseek-r1:1.5b...
runner PSS: ~1196 MiB
runner Private_Dirty: ~1178 MiB
runner maps:
  1000000000-1020000000 rw-s /memfd:scudo_arena_0
  1020000000-1040000000 rw-s /memfd:scudo_arena_1
  ...
  10e0000000-1100000000 rw-s /memfd:scudo_arena_7
  /data/local/tmp/md/libscudo_md_android.so
  [anon:scudo:secondary]
```

Ollama 日志中出现 SharedArena 分配/释放路径：

```text
[scudo][shared_arena] allocate hit core=1 request_size=607744 ...
[scudo][shared_arena] deallocate return core=0 ...
[scudo][shared_arena] retrieve core=0 request_size=3633288 ...
[scudo][shared_arena] trace limit reached, suppressing further logs
```

当前结论：不用重编 Ollama，修复后的 `LD_PRELOAD` 方案已经能把现成
Termux/Ollama server 与 runner 接入项目修改版 Scudo，并成功 attach
Android broker / SharedArena。

## 6. 阶段 3：Ollama workload 执行

已解决 allocator 接入问题，现成 Termux/Ollama 可通过修复后的
`LD_PRELOAD=/data/local/tmp/md/libscudo_md_android.so` 进入本项目修改版 Scudo。

尚未执行正式 baseline/shared 对比；下一轮应基于 5.6 的启动方式，分别运行：

- baseline：`LD_PRELOAD=/data/local/tmp/md/libscudo_md_android.so` +
  `SCUDO_SHARED_ARENA_FORCE=0`
- SharedArena：`LD_PRELOAD=/data/local/tmp/md/libscudo_md_android.so` +
  `SCUDO_SHARED_ARENA_FORCE=1`

注意两组都应使用同一个 preload allocator，这样差异才来自 SharedArena 开关，而不是
系统 bionic malloc 与 Scudo allocator 本身的差异。

## 7. 阶段 4：结果汇总

当前结论：

- 手机内核、debugfs、broker、Scudo SharedArena correctness 链路正常。
- Ollama 1.5B 本地推理可运行；模型加载日志显示 `mmap=false`，CPU model buffer
  约 1059.89 MiB，runner PSS 约 1117 MiB，主要是大块匿名 `[anon:libc_malloc]`。
- 现成 Termux/Ollama 原本不会响应 `SCUDO_SHARED_ARENA_FORCE=1`，因为没有链接项目
  修改过的 Scudo。
- 修复 `LD_PRELOAD` Scudo 后，现成 Termux/Ollama server 和 runner 均可 attach
  broker，并映射 SharedArena 固定基址 `0x1000000000` 起的 8 个 per-core arena。

下一步可行路径：

1. 基于 `LD_PRELOAD` 方案补一个自动化 runner 脚本，分别跑 baseline/shared。
2. 增加采样脚本，统计 `scudo:secondary`、SharedArena memfd、PSS/private、broker
   serve 记录和 Ollama 日志中的 `allocate hit/retrieve/store`。
3. 做 sequential-wave / concurrent-then-free 两组正式 workload。

## 8. 阶段 5：正式 baseline / SharedArena 性能对比

执行时间：2026-05-26 17:42-17:50 CST。

### 8.1 自动化脚本

新增脚本：

```text
tools/testing/memory_delegation/scripts/run_phone_ollama_preload_compare.sh
```

脚本口径：

- 两组都使用同一个 `LD_PRELOAD=/data/local/tmp/md/libscudo_md_android.so`。
- baseline 使用 `SCUDO_SHARED_ARENA_FORCE=0`。
- SharedArena 使用 `SCUDO_SHARED_ARENA_FORCE=1`。
- 两组都启动 `memory_delegation_broker`，保证 attach 环境一致。
- 启动 Ollama 时不设置 Termux `LD_LIBRARY_PATH`，避免 `libunwindstack.so` 错拿
  Termux `liblzma.so`。
- 每组结束后采集：
  - `/proc/meminfo` 的 `MemAvailable`
  - Ollama server + runner 的 `smaps_rollup`
  - `scudo:secondary` 映射 PSS/private/shared
  - `memfd:scudo_arena_*` 映射 PSS/private/shared
  - broker fd serve 次数

构建并运行命令：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

MODEL='deepseek-r1:1.5b' \
NUM_CTX=512 \
NUM_PREDICT=32 \
REPEAT=2 \
tools/testing/memory_delegation/scripts/run_phone_ollama_preload_compare.sh
```

输出日志：

```text
/tmp/md-phone-run/ollama-preload-compare-20260526-174559.txt
```

补充单次预跑日志：

```text
baseline: /tmp/md-phone-run/ollama-preload-compare-20260526-174224.txt
shared:   /tmp/md-phone-run/ollama-preload-compare-20260526-174346.txt
```

### 8.2 workload

模型：

```text
deepseek-r1:1.5b
```

请求：

```json
{
  "model": "deepseek-r1:1.5b",
  "prompt": "用一句话回答：1+1等于几？",
  "stream": false,
  "keep_alive": -1,
  "options": {
    "num_predict": 32,
    "num_ctx": 512
  }
}
```

Ollama 日志显示两组模型加载口径一致：

```text
load_tensors: loading model tensors, this can take a while... (mmap = false)
load_tensors:          CPU model buffer size =  1059.89 MiB
llama_kv_cache:        CPU KV buffer size =    14.00 MiB
llama_context:        CPU compute buffer size =   299.75 MiB
```

### 8.3 生成时延

`total_duration_ns` 来自 Ollama `/api/generate` 返回 JSON；`wall_s` 是手机侧
`date +%s` 粗粒度墙钟。

| 模式 | iter | wall | total | load | prompt eval | decode eval |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1 | 84 s | 83.925 s | 8.578 s | 4.608 s | 70.610 s |
| baseline | 2 | 89 s | 88.471 s | 0.827 s | 3.844 s | 83.701 s |
| SharedArena | 1 | 103 s | 103.232 s | 8.551 s | 4.716 s | 89.818 s |
| SharedArena | 2 | 105 s | 105.108 s | 0.821 s | 3.565 s | 100.562 s |

对比：

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| iter1 total | 83.925 s | 103.232 s | 慢 23.0% |
| iter2 total | 88.471 s | 105.108 s | 慢 18.8% |
| iter2 decode eval | 83.701 s | 100.562 s | 慢 20.1% |

结论：在当前 Ollama 1.5B 单 runner workload 中，SharedArena 路径有明显推理时延
开销。由于 iter2 已基本排除模型加载，开销主要体现在 decode/eval 阶段。

### 8.4 live 内存

采样点：两次 generate 结束后，`keep_alive=-1` 保持 runner 存活。

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| `MemAvailable` drop | 1176.3 MiB | 1231.2 MiB | 多 55.0 MiB |
| group RSS | 1338.4 MiB | 1346.2 MiB | 多 7.9 MiB |
| group PSS | 1259.1 MiB | 1265.7 MiB | 多 6.7 MiB |
| group private | 1227.5 MiB | 1233.6 MiB | 多 6.1 MiB |
| `scudo:secondary` PSS | 1113.6 MiB | 1059.9 MiB | 少 53.8 MiB |
| SharedArena PSS | 0.03 MiB | 59.6 MiB | 多 59.6 MiB |
| allocator PSS (`secondary + arena`) | 1113.7 MiB | 1119.5 MiB | 多 5.8 MiB |
| `scudo:secondary` map count | 58 | 1 | 少 57 |
| SharedArena map count | 16 | 16 | 不变 |

结论：

- SharedArena 已经接管了一部分大块分配，`scudo:secondary` PSS 下降约 53.8 MiB。
- 但这部分基本转移到了 SharedArena，SharedArena PSS 增加约 59.6 MiB。
- 总 allocator PSS 没有下降，反而略增约 5.8 MiB。
- 总体 group PSS/private 和系统 `MemAvailable` drop 均没有改善。

### 8.5 关键解释

这轮结果不代表 broker/SharedArena 没接上；接入已经成立：

```text
broker_serve_count=27
arena_map_count=16
runner maps include:
  1000000000-1020000000 /memfd:scudo_arena_0
  ...
  10e0000000-1100000000 /memfd:scudo_arena_7
```

真正的问题是当前 Ollama workload 的最大内存来源是：

```text
CPU model buffer size = 1059.89 MiB
```

而当前每个 per-core arena 映射大小是：

```text
0x20000000 = 512 MiB
```

单个 1059.89 MiB 模型权重 buffer 超过单个 per-core arena 容量，因此无法由当前
`SharedArena::retrieve()` 满足，最终仍回退到普通 Scudo Secondary。SharedArena
只承载了部分较小的 runtime allocation，约 60 MiB。

这与 DESIGN.md 中“arena_size 作为全局常量、预留足够大的连续 VA 区间”的设计目标
存在一个工程参数不匹配：端侧 LLM 模型权重的一次性大块 allocation 已经超过当前
per-core arena size。

### 8.6 当前结论

在 `deepseek-r1:1.5b`、`num_ctx=512`、`num_predict=32`、单 Ollama runner 场景下：

- 修改版 Scudo 已能通过 `LD_PRELOAD` 接入现成 Termux/Ollama。
- SharedArena attach、broker fd 分发、固定 VA 映射均正常。
- 当前 per-core arena size 无法容纳 Ollama 的 1059.89 MiB CPU model buffer。
- 因此没有观察到端侧 LLM live 内存节省。
- SharedArena 路径在该 workload 中引入约 19-23% 推理时延开销。

下一步建议：

1. 若目标是验证 LLM 场景内存节省，需要把 per-core arena size 提高到至少覆盖
   1.1 GiB 级别 allocation，或支持单 allocation 跨 arena/chunk。
2. 若目标仍是“进程私有 cache 打通”，应构造多 runner sequential-wave，并让每个
   runner 的大块 allocation 能进入 SharedArena；否则模型主 buffer 留在 Secondary，
   对比会被 1GB 级 fallback 淹没。
3. 后续正式论文/专利数据不建议直接使用当前配置下的 Ollama 结果作为正向收益数据；
   它更适合作为“端侧 LLM 暴露 arena_size 参数瓶颈”的诊断结果。

### 8.7 收尾状态

本轮测试结束后，跳板机侧 `adb devices -l` 已经看不到手机：

```text
List of devices attached
```

因此未能做最后一次手机侧 `ps` 进程复核。自动化脚本在每组 workload 结束时都会执行
`kill_all`，清理 `ollama` 和 `memory_delegation_broker`；但由于手机已经与跳板机
断开，本轮文档只记录脚本行为，最终设备状态未复核。

## 9. 阶段 6：SharedArena 推理慢的原因复核

### 9.1 问题

阶段 5 中 baseline 比 SharedArena 快约 19-23%，并且 iter2 已经基本排除模型加载，
慢点主要落在 `decode eval`。本阶段在手机重新连接后复测，目标是区分：

- SharedArena 热路径本身是否稳定造成 20% 级别退化。
- 固定 baseline 先跑、SharedArena 后跑是否受到温度、频率、页缓存、后台状态影响。
- 首次模型加载和 steady-state decode 是否应分开分析。

手机重新连接后确认：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  "/opt/homebrew/bin/adb devices -l && \
   /opt/homebrew/bin/adb -s 3B15AL00K5D00000 shell getprop sys.boot_completed"
```

输出显示设备在线，`sys.boot_completed=1`。

### 9.2 baseline -> SharedArena 复测

命令：

```bash
BUILD_PRELOAD=0 \
RUN_BASELINE=1 \
RUN_SHARED=1 \
REPEAT=2 \
NUM_PREDICT=32 \
NUM_CTX=512 \
MODEL='deepseek-r1:1.5b' \
tools/testing/memory_delegation/scripts/run_phone_ollama_preload_compare.sh
```

日志：

```text
/tmp/md-phone-run/ollama-preload-compare-20260528-115155.txt
```

结果：

| 模式 | iter | total | load | prompt eval | decode eval | eval_count |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | 1 | 17.234 s | 11.330 s | 3.486 s | 2.389 s | 32 |
| baseline | 2 | 3.227 s | 0.281 s | 0.072 s | 2.851 s | 32 |
| SharedArena | 1 | 8.091 s | 1.765 s | 3.414 s | 2.884 s | 32 |
| SharedArena | 2 | 2.448 s | 0.215 s | 0.119 s | 2.090 s | 32 |

本轮没有复现阶段 5 的 20% 级别退化；warm iter2 中 SharedArena 反而更快。

内存状态仍符合预期：

| 指标 | baseline | SharedArena |
| --- | ---: | ---: |
| `scudo:secondary` PSS | 1098.1 MiB | 1085.3 MiB |
| SharedArena PSS | 0.03 MiB | 76.1 MiB |
| `scudo:secondary` map count | 58 | 1 |

说明 SharedArena 确实承载了部分 runtime allocation，但主模型 buffer 仍在
`scudo:secondary`。

### 9.3 SharedArena -> baseline 反向顺序复测

为提高 decode 占比，将 `num_predict` 提到 64，并先跑 SharedArena，再跑 baseline。

命令：

```bash
RUN_BASELINE=0 RUN_SHARED=1 BUILD_PRELOAD=0 \
REPEAT=3 NUM_PREDICT=64 NUM_CTX=512 MODEL='deepseek-r1:1.5b' \
tools/testing/memory_delegation/scripts/run_phone_ollama_preload_compare.sh

RUN_BASELINE=1 RUN_SHARED=0 BUILD_PRELOAD=0 \
REPEAT=3 NUM_PREDICT=64 NUM_CTX=512 MODEL='deepseek-r1:1.5b' \
tools/testing/memory_delegation/scripts/run_phone_ollama_preload_compare.sh
```

日志：

```text
/tmp/md-phone-run/ollama-preload-compare-20260528-115316.txt
/tmp/md-phone-run/ollama-preload-compare-20260528-115411.txt
```

结果：

| 模式 | iter | total | load | prompt eval | decode eval | eval_count |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| SharedArena | 1 | 30.252 s | 8.598 s | 4.760 s | 16.832 s | 64 |
| SharedArena | 2 | 5.165 s | 0.202 s | 0.156 s | 4.764 s | 64 |
| SharedArena | 3 | 6.018 s | 0.235 s | 0.166 s | 5.573 s | 64 |
| baseline | 1 | 6.839 s | 1.553 s | 0.242 s | 4.996 s | 64 |
| baseline | 2 | 4.914 s | 0.243 s | 0.079 s | 4.547 s | 64 |
| baseline | 3 | 5.798 s | 0.183 s | 0.161 s | 5.410 s | 64 |

steady-state 只看 iter2/iter3：

| 指标 | baseline | SharedArena | 变化 |
| --- | ---: | ---: | ---: |
| avg total | 5.356 s | 5.592 s | SharedArena 慢 4.4% |
| avg decode eval | 4.979 s | 5.169 s | SharedArena 慢 3.8% |
| avg decode/token | 77.8 ms | 80.8 ms | SharedArena 慢 3.0 ms/token |

SharedArena 首次 iter1 明显慢，但 baseline 是在 SharedArena 之后运行，模型文件页缓存已经
变热；baseline 日志显示 runner 启动约 0.93 s，而 SharedArena 首次 runner 启动约
5.65 s。因此 iter1 不适合直接作为 allocator 对比。

### 9.4 设备状态采样

复测后采样 CPU 频率和温度：

```bash
ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes lrc@192.168.61.230 \
  '/opt/homebrew/bin/adb -s 3B15AL00K5D00000 shell su -c '"'"'
    echo cpu_freq
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
      printf "%s=" "$f"; cat "$f" 2>/dev/null
    done
    echo thermal
    for z in /sys/class/thermal/thermal_zone*/temp; do
      typefile=${z%/temp}/type
      printf "%s=" "$(cat "$typefile" 2>/dev/null)"
      cat "$z" 2>/dev/null
    done
  '"'"''
```

采样时 CPU 处于空闲低频：

```text
cpu0-5: 384000
cpu6-7: 883200
主要 thermal zone: 约 29-31 C
```

该采样不代表推理中的实时频率，但说明复测结束后设备没有处于明显高温状态。

### 9.5 原因判断

本轮复测后，阶段 5 的 “SharedArena 稳定慢 19-23%” 不能成立为可复现结论。
更可能的解释是当时测试受到设备运行状态影响，例如温度/频率、后台负载、内存压力、
模型文件页缓存冷热，以及固定运行顺序。

当前可复现的性能画像是：

- 主要耗时瓶颈是 Ollama/llama.cpp 的 CPU decode eval，而不是模型加载或 broker。
- SharedArena steady-state decode 只表现出约 4% 的小幅开销。
- 这部分小幅开销来自 SharedArena 接管的 runtime allocation：`scudo:secondary`
  map count 从约 58/59 降到 1，SharedArena PSS 增至约 68-78 MiB。
- 主模型权重 buffer 仍是约 1059.89 MiB 单次 CPU backend allocation，超过当前
  per-core arena 512 MiB，因此仍落在普通 Scudo Secondary。

代码层面的瓶颈路径是 Secondary 大块分配被强制尝试 SharedArena：

- `MapAllocator::allocate()` 在 `SCUDO_SHARED_ARENA_FORCE=1` 时调用
  `SharedArenaPool::shouldUseArena()`，随后尝试 `tryAllocateFromArena()`。
- `SharedArena::retrieve()` 需要获取 per-core 共享 spinlock，扫描 freelist 或 bump
  分配，并向共享 log ring 追加 alloc 记录。
- `MapAllocator::deallocate()` 对 arena 地址调用 `SharedArena::store()`，同样需要
  共享锁、按 VA 插入/合并 freelist，并追加 free 记录。

这些路径会让被 SharedArena 承载的中等大块 allocation 比普通 Scudo Secondary 多一些
共享元数据和同步成本；但本轮 steady-state 数据显示它不是 20% 级别退化的主因。

### 9.6 更新结论

Ollama 当前仍不适合作为 SharedArena 正向收益主 workload：

- 最大内存来源是仍被应用持有的 1.06 GiB 模型权重 live buffer。
- 该 buffer 超过 per-core arena 容量，无法进入 SharedArena。
- 被 SharedArena 接管的是约 70 MiB runtime allocation，内存收益不足以抵消实验噪声。

但阶段 5 的性能负结论需要下调：当前复测显示 SharedArena 在 warm decode 下只有约
4% 开销，之前 19-23% 的差距应视为未控制设备状态和运行顺序时的异常结果。
