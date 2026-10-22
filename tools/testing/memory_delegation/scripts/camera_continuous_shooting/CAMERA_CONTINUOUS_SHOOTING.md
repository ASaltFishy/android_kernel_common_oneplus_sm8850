# OnePlus camera continuous shooting experiment

本文档记录当前相机普通 baseline/default、delegated 压力连拍矩阵，以及 allocator CSV trace/replay 的推荐跑法。这里保留当前真实测试需要的主路径；旧的 heapprofd、Perfetto、bucket smoke、零散 apps=4 单点排查等调试尝试不再维护。

## 1. 连接与基础环境

在服务器侧进入内核 common 目录，并设置 ADB 环境：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

export ADB_SERVER_SOCKET=tcp:127.0.0.1:15037
export ADB_BIN=adb
export ADB_SERIAL=3B15AL00K5D00000
```

手机通过 Mac 上的 adb server 反向转发到服务器本机 `127.0.0.1:15037`。Mac 侧保持隧道在线：

```bash
adb start-server
ssh -N \
  -o ServerAliveInterval=30 \
  -o ServerAliveCountMax=3 \
  -o ExitOnForwardFailure=yes \
  -R 127.0.0.1:15037:127.0.0.1:5037 \
  lrc-ubuntu
```

检查连接和 boot id：

```bash
$ADB_BIN -s "$ADB_SERIAL" wait-for-device
$ADB_BIN -s "$ADB_SERIAL" shell getprop sys.boot_completed
$ADB_BIN -s "$ADB_SERIAL" shell 'cat /proc/sys/kernel/random/boot_id; cat /proc/uptime'
```

## 2. 恢复 Camera Scudo overlay

相机通过 KernelSU systemless overlay 加载修改版 Scudo。PC 侧执行恢复脚本即可，脚本会构建并推送 wrapper/preload so：

```bash
ADB_BIN="$ADB_BIN" ADB_SERIAL="$ADB_SERIAL" BUILD=1 \
  tools/testing/memory_delegation/scripts/camera_continuous_shooting/recover_phone_camera_scudo_overlay.sh
```

常用 property：

```text
debug.md.camera_scudo.mode=default    # 修改版 Scudo 默认路径，禁用 SharedArena
debug.md.camera_scudo.mode=delegated  # 修改版 Scudo + memory delegation
debug.md.camera_scudo.alloc_stats=0   # 正式测试保持关闭
debug.md.camera_scudo.stats_interval=0
```

确认 delegated 模式是否生效：

```bash
ADB_BIN="$ADB_BIN" ADB_SERIAL="$ADB_SERIAL" \
  bash tools/testing/memory_delegation/scripts/camera_continuous_shooting/check_phone_camera_scudo_arena.sh
```

delegated 生效时应能看到 `SCUDO_SHARED_ARENA_FORCE=1` 和 `/dev/ashmem/scudo_arena_*` 映射。

## 3. 真实相机连拍压力测试

正式连拍性能对比使用 `run_phone_camera_pressure_bench.sh`。脚本会生成手机端脚本并 push 到 `/data/local/tmp/md_camera/run_camera_pressure_phone.sh` 后执行，不需要在 `adb shell` 里粘大段命令。

推荐一键跑 baseline/default 与 delegated：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=1 \
RUN_BASELINE_DELEGATED_SUITE=1 \
SUITE_LABEL_PREFIX=scudo-baseline-delegated-clean \
SUITE_PRESSURE_APPS="0 1 2" \
SUITE_PRESSURE_MB_PER_APP=256 \
ROUNDS=2 TARGET_PHOTOS=30 \
SHOOT_MODE=tap TAP_INTERVAL_MS=120 \
SETTLE_SECS=10 POST_WAIT_SECS=5 \
PHOTO_POLL_EVERY_ATTEMPTS=5 \
GROUP_CLEANUP=1 \
CLEAN_PHOTOS_BETWEEN_GROUPS=1 \
CLEAN_PHOTOS_BETWEEN_ROUNDS=1 \
ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS=1 \
RESTART_BROKER_ON_START=1 \
RESTART_BROKER_BETWEEN_GROUPS=0 \
DISMISS_USB_DIALOG=1 \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_camera_pressure_bench.sh
```

输出位置：

```text
/tmp/md-camera-run/camera-pressure-<VARIANT_LABEL>-*.csv
/tmp/md-camera-run/camera-pressure-<VARIANT_LABEL>-*.png
/tmp/md-camera-run/camera-pressure-<VARIANT_LABEL>-*.log
/tmp/md-camera-run/md-camera-*/run_camera_pressure_phone.sh
```

关键判读字段：

```text
photo_target_met,focus_ok,photos,elapsed_s,photos_per_s,drain_s
donor_ready,donor_arena_allocs,donor_secondary_pss_kb
pss_before_kb,pss_after_kb
scudo_secondary_pss_before_kb,scudo_secondary_pss_after_kb
arena_pss_before_kb,arena_pss_after_kb
```

有效数据要求：

- `photo_target_met=1`：完成目标张数。
- `focus_ok=1`：相机界面确实在前台。
- `donor_ready == pressure_apps`：后台压力完整生效。
- 测试前后 boot id 不变；若手机重启，该轮只用于稳定性排查。
- delegated 组需要关注 `donor_arena_allocs` 是否随 donor 数增加，以及
  `donor_secondary_pss_kb` 是否接近 0。
- dmesg 中不应出现新的 `memory_delegation: log dropped observed`；若出现，
  该轮性能数据只能作为趋势参考，不能视为 correctness clean。

## 4. 单点 delegated smoke

只验证 delegated 相机路径是否稳定时，可以跑一轮 apps `0/1/2`：

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=1 \
VARIANT_LABEL=scudo-delegated-smoke \
BACKGROUND_MODE=donor DONOR_MODE=delegated \
PRESSURE_APPS="0 1 2" PRESSURE_MB_PER_APP=256 \
DONOR_CHUNK_KB=1024 DONOR_CHUNKS=0 DONOR_CPU_MODE=spread \
ROUNDS=1 TARGET_PHOTOS=30 \
SHOOT_MODE=tap TAP_INTERVAL_MS=120 \
SETTLE_SECS=10 POST_WAIT_SECS=5 \
GROUP_CLEANUP=1 \
CLEAN_PHOTOS_BETWEEN_GROUPS=1 \
CLEAN_PHOTOS_BETWEEN_ROUNDS=1 \
CLEAN_PHOTOS_ON_EXIT=1 \
FINAL_PHOTO_CLEANUP_WAIT_SECS=5 \
ARCHIVE_ARENA_LOGS_BETWEEN_GROUPS=1 \
RESTART_BROKER_ON_START=1 \
RESTART_BROKER_BETWEEN_GROUPS=0 \
DISMISS_USB_DIALOG=1 \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_camera_pressure_bench.sh
```

## 5. 查看最近结果

查看最近相机连拍输出：

```bash
ls -lt /tmp/md-camera-run | head -n 20
```

正式对比时优先保留这些文件：

```text
camera-pressure-*.csv
camera-pressure-*.log
```

## 6. Scudo Secondary-only trace 录制

allocator trace 使用修改版 Scudo 自带的 CSV recorder，不依赖 heapprofd/Perfetto。恢复相机 overlay 后打开 property：

```bash
ADB_BIN="$ADB_BIN" ADB_SERIAL="$ADB_SERIAL" \
  $ADB_BIN -s "$ADB_SERIAL" shell 'su -c "setprop debug.md.camera_scudo.alloc_trace 1; setprop debug.md.camera_scudo.alloc_trace_max_events 500000"'
```

然后按第 3 节跑一轮真实相机连拍。相机进程会在应用 cache 下生成：

```text
/data/data/com.oplus.camera/cache/md_camera_alloc_trace.csv
```

脚本归档后，服务器侧常见位置为：

```text
/tmp/md-camera-run/alloc-trace-*/md_camera_alloc_trace.csv
```

CSV 字段：

```text
seq,ts_ns,tid,cpu,op,ptr,old_ptr,req_size,nmemb,alignment,zero_requested,usable_size,result
```

录制完成后正式性能测试建议关闭 trace：

```bash
ADB_BIN="$ADB_BIN" ADB_SERIAL="$ADB_SERIAL" \
  $ADB_BIN -s "$ADB_SERIAL" shell 'su -c "setprop debug.md.camera_scudo.alloc_trace 0"'
```

## 7. 单次 trace replay

单次 replay 用最近一次 `md_camera_alloc_trace.csv`，也可以显式传入 `TRACE_FILE=/path/to/md_camera_alloc_trace.csv`。

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=1 \
MODE=delegated TOUCH=none MAX_EVENTS=0 \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_alloc_trace_replay.sh
```

验证每页触碰成本时使用：

```bash
ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=0 \
MODE=delegated TOUCH=pages MAX_EVENTS=0 DELEGATED_RESET_ON_INIT=1 \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_alloc_trace_replay.sh
```

delegated 单次 replay 默认 `DELEGATED_RESET_ON_INIT=1`，用于 broker/arena 重启后清理内核 ownership truth。旧内核不认识 reset flag 时，Scudo 会走兼容 fallback，但完整正确性验证需要新内核。

## 8. replay 压力矩阵

正式 trace replay 压力矩阵使用 default/delegated 对比。脚本会生成手机端脚本再执行，不在 adb shell 中粘大段命令。

```bash
cd /home/lrc/patent/kernel/kernel_platform/common

ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=0 \
PRESSURE_APPS="0 1 2 3 4" \
TOUCH=none \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_alloc_trace_pressure_matrix.sh
```

每组重复三次并汇总平均值：

```bash
ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=0 \
RUNS=3 PRESSURE_APPS="0 1 2 3 4" \
TOUCH=none \
LOCAL_OUT_DIR=/tmp/md-alloc-trace-pressure-matrix-$(date +%Y%m%d-%H%M%S) \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_alloc_trace_pressure_matrix_repeated.sh
```

每页触碰模式：

```bash
ADB_SERVER_SOCKET=tcp:127.0.0.1:15037 \
ADB_BIN=adb ADB_SERIAL=3B15AL00K5D00000 BUILD=0 \
RUNS=3 PRESSURE_APPS="0 1 2 3 4" \
TOUCH=pages DELEGATED_TOUCH_DELAY_US=0 \
LOCAL_OUT_DIR=/tmp/md-alloc-trace-pressure-matrix-touch-pages-$(date +%Y%m%d-%H%M%S) \
tools/testing/memory_delegation/scripts/camera_continuous_shooting/run_phone_alloc_trace_pressure_matrix_repeated.sh
```

关键输出：

```text
matrix_results.csv
summary_avg.csv
baseline_vs_delegated_avg.csv
```

正确性要求：

- 所有行 `rc=0`、`alloc_failures=0`。
- `donor_ready == apps`。
- delegated pressure 日志中 `arena_allocs` 应随 donor 数出现，default 对照应为 0。
- 本轮 dmesg 不应出现新的 `deny arena access`、`EBUSY`、Oops、panic、SIGSEGV。
