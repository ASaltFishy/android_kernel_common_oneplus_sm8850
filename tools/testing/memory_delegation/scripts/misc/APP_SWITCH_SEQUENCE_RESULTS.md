# Multi-app app-switch sequence results

Date: 2026-06-05

## Setup

- Device: PLK110
- Screen: 1272x2772
- SharedArena live overlay: enabled with per-package `wrap.<pkg>=/system/bin/bugreportz`
- Target packages:
  - `tv.danmaku.bili`
  - `com.tencent.qqmusic`
  - `com.taobao.taobao`
  - `com.tencent.qqlive`
  - `com.alibaba.wireless`
  - `com.baidu.searchbox`
  - `com.dragon.read`
  - `com.autonavi.minimap`
  - `com.sina.weibo`
  - `com.xingin.xhs`
  - `com.ss.android.ugc.aweme`

## Run 1

The action sequence completed, but result-file generation failed while decoding
non-UTF-8 bytes from full `logcat -d`.

| Step | App | LaunchState | WaitTime ms | TotalTime ms |
| --- | --- | --- | ---: | ---: |
| r1_bili_start | Bilibili | COLD | 1076 | 1069 |
| r1_qqmusic_start | QQ Music | COLD | 643 | 632 |
| r1_taobao_background | Taobao | COLD | 876 | 870 |
| r1_qqlive_background | Tencent Video | COLD | 753 | 746 |
| r1_alibaba_background | Alibaba | COLD | 998 | 987 |
| r1_baidu_background | Baidu | COLD | 1312 | 1305 |
| r1_tomato_background | Tomato Novel | COLD | 931 | 928 |
| r1_amap_background | AMap | COLD | 703 | 699 |
| r1_weibo_background | Weibo | COLD | 1262 | 1255 |
| r1_xhs_foreground | Xiaohongshu | COLD | 914 | 904 |
| r1_xhs_to_douyin | Douyin | COLD | 1347 | 1340 |

## Run 2

The sequence progressed until Douyin, but `am start -W` stopped returning
parseable `Status` / `WaitTime` fields from Tomato Novel onward. The script then
timed out while collecting full logcat. Shortly after this run, adb disappeared;
when the device returned, uptime was about 1 minute and `ro.boot.bootreason` was
`reboot`.

Partial timings:

| Step | App | LaunchState | WaitTime ms | TotalTime ms |
| --- | --- | --- | ---: | ---: |
| r1_bili_start | Bilibili | COLD | 1151 | 1146 |
| r1_qqmusic_start | QQ Music | COLD | 521 | 519 |
| r1_taobao_background | Taobao | COLD | 804 | 799 |
| r1_qqlive_background | Tencent Video | COLD | 756 | 754 |
| r1_alibaba_background | Alibaba | COLD | 831 | 828 |
| r1_baidu_background | Baidu | COLD | 897 | 893 |

## Notes

- The sequence script was updated to decode command output with replacement
  characters and to collect bounded logcat output (`-t N`).
- Results are not yet considered stable because the multi-app SharedArena
  preload run caused a device reboot.

## Run 3

Command:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1
```

Output directory:

```text
/tmp/md-phone-run/app-switch-20260605-160617
```

The background setup sequence completed and memory was sampled immediately after
Weibo was moved to the background. The device disappeared from adb during the
Xiaohongshu -> Douyin switch; after it returned, uptime was about 1 minute and
`ro.boot.bootreason` was `reboot`.

| Step | App | LaunchState | WaitTime ms | TotalTime ms |
| --- | --- | --- | ---: | ---: |
| r1_bili_start | Bilibili | COLD | 872 | 866 |
| r1_qqmusic_start | QQ Music | COLD | 510 | 505 |
| r1_taobao_background | Taobao | COLD | 815 | 804 |
| r1_qqlive_background | Tencent Video | COLD | 730 | 723 |
| r1_alibaba_background | Alibaba | COLD | 776 | 773 |
| r1_baidu_background | Baidu | COLD | 1004 | 999 |
| r1_tomato_background | Tomato Novel | COLD | 920 | 911 |
| r1_amap_background | AMap | COLD | 716 | 711 |
| r1_weibo_background | Weibo | COLD | 1343 | 1334 |
| r1_xhs_foreground | Xiaohongshu | COLD | 962 | 957 |
| r1_xhs_to_douyin | Douyin | invalid | N/A | N/A |

Memory after background setup:

| Metric | Value MB |
| --- | ---: |
| MemTotal | 11140.1 |
| MemFree | 345.1 |
| MemAvailable | 2704.4 |
| Cached | 4212.4 |
| SwapTotal | 9728.0 |
| SwapFree | 4516.9 |
| AnonPages | 2595.1 |
| Mapped | 2870.2 |
| Slab | 716.5 |
| PageTables | 222.0 |

Per-package memory after background setup:

| Package | Processes | PSS MB | RSS MB | Private Dirty MB | Private Clean MB | SwapPss MB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| tv.danmaku.bili | 3 | 516.7 | 665.7 | 68.3 | 19.3 | 373.3 |
| com.taobao.taobao | 3 | 653.4 | 778.1 | 208.3 | 41.9 | 350.7 |
| com.alibaba.wireless | 3 | 314.5 | 593.8 | 87.2 | 17.1 | 184.2 |
| com.baidu.searchbox | 2 | 567.9 | 839.0 | 244.8 | 44.3 | 189.7 |
| com.dragon.read | 3 | 898.4 | 1399.4 | 358.0 | 156.9 | 237.9 |
| com.autonavi.minimap | 4 | 1270.7 | 1722.3 | 905.8 | 90.9 | 215.8 |
| com.sina.weibo | 2 | 460.8 | 665.3 | 172.9 | 44.9 | 170.0 |

QQ Music and Tencent Video were not present in `ps -A` at the memory sampling
point, so they had already exited or been reclaimed before the background setup
snapshot.

## Stability convergence plan

The current SharedArena app-switch data must be treated as a stability
investigation first, not as an LMKD comparison yet. Run 2 and Run 3 ended with
an unexpected device reboot, while normal LMKD background reclamation should not
reboot the phone.

The app-switch runner now records boot markers and collects post-failure
diagnostics automatically. A normal SharedArena repro command is:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --collect-reboot-diagnostics
```

For every run, inspect these files in the printed `out_dir`:

```text
pre_boot_marker.txt
post_boot_marker.txt
failure.txt
launch_times.csv
am_start_raw.txt
reboot-diagnostics-failure/summary.txt
reboot-diagnostics-failure/pstore.txt
reboot-diagnostics-failure/dmesg.txt
reboot-diagnostics-failure/logcat_interesting.txt
reboot-diagnostics-failure/tombstones.txt
reboot-diagnostics-failure/shared_arena_logs.txt
```

Interpretation:

- `boot_id_changed=1` in `summary.txt` confirms a real reboot during the run.
- `pstore.txt` containing panic/oops/watchdog text points to a kernel or vendor
  watchdog path.
- `tombstones.txt` or `logcat_interesting.txt` showing `system_server`,
  `zygote`, linker, Scudo, or wrapped-app aborts points to userspace crash or
  preload incompatibility.
- `shared_arena_logs.txt` should be checked for broker failures, repeated
  attach failures, suspicious allocator traces, or log truncation before the
  reboot.

Only after the SharedArena overlay can complete this sequence repeatedly without
reboot should we resume LMKD evaluation. The next LMKD comparison should use the
same action sequence for baseline allocator and SharedArena allocator, with the
only intended variable being the allocator/preload path.

## Run 4 - stability repro with diagnostics

Date: 2026-06-10

First, the app-switch SharedArena overlay was restored with:

```bash
tools/testing/memory_delegation/scripts/kernelsu/recover_phone_app_switch_scudo_overlay.sh
```

The recovery built and pushed `libscudo_md_preload.so` plus
`memory_delegation_broker`, then configured all target package
`wrap.<pkg>` properties to `/system/bin/bugreportz`. The broker reported:

```text
[memory_delegation_broker] ready num_cores=8
```

A resolve-only precheck then passed:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --resolve-only \
  --require-shared-arena-wrap \
  --out-dir /tmp/md-phone-stability-precheck
```

All target launcher activities resolved and all target `wrap.<pkg>` properties
were set to `/system/bin/bugreportz`.

The first full stability run was:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --collect-reboot-diagnostics \
  --out-dir /tmp/md-phone-stability-run
```

Output directory:

```text
/tmp/md-phone-stability-run/app-switch-20260610-114322
```

The run launched Bilibili successfully, then the first scripted tap timed out:

```text
r1_bili_start: LaunchState=COLD WaitTime=1166ms TotalTime=1160ms
TimeoutExpired: adb shell input tap 318 942 timed out after 10 seconds
```

The phone then rebooted. Manual post-reboot check showed:

```text
pre_boot_id=7a0494f3-ee2a-4051-9267-12b951cbdc06
post_boot_id=b8ded189-7898-40ca-af76-999a102913d4
uptime after return ~= 65s
ro.boot.bootreason=reboot
```

Diagnostics:

- `pstore` was empty after reboot.
- `logcat_all.txt` contains normal bootstat reason `reboot`.
- `logcat_all.txt` also contains the vendor minidump marker:

```text
minidumpreader: fatal = 1, panic = true, silence = 0, ftmmode = -1
minidumpreader: minidump  read end size = 0
```

Current interpretation:

- This is not an LMKD event. It is a fatal reboot during the first wrapped app
  run.

## Run 5 - system malloc_debug path convergence

Date: 2026-06-10

The `wrap.<pkg>=/system/bin/bugreportz` path was found unreliable for commercial
apps on this ROM: the wrap property can be present while the target process maps
do not contain the preload library or SharedArena mappings. Therefore the next
tests used Android bionic's `libc_malloc_debug.so` dispatch path.

Script fixes made during this convergence pass:

- `run_phone_app_switch_sequence.py` no longer pulls full `/proc/<pid>/maps` for
  preload checks. It now greps only `libc_malloc_debug`, `libscudo`, and
  `scudo_arena_` lines with a short timeout, and records `check_error` instead
  of failing the whole run.
- `recover_system_scudo_overlay_phone.sh` now unmounts the previous live bind
  mount before deleting the work directory, and uses lazy unmount as a fallback.
  Without this, repeated live recovery could delete the old bind-mount source
  and leave `/apex/.../libc_malloc_debug.so` in a dirty state.
- `recover_phone_system_scudo_overlay.sh` now forwards `PROGRAM`, allowing
  filtered tests such as `PROGRAM=tv.danmaku.bili` or `PROGRAM=app_process`.

Useful observations:

- Shell/native smoke processes do load the adapter when
  `libc.debug.malloc.options=md_shared` is set. The smoke maps contained
  `/apex/com.android.runtime/lib64/libc_malloc_debug.so` and all eight
  `/dev/ashmem/scudo_arena_*` mappings.
- A global app-switch rerun with lightweight maps checks completed without a
  reboot:

```text
out_dir=/tmp/md-phone-system-scudo-global-bg1-rerun/app-switch-20260610-121027
lmkd_kill_hits=0
```

  However every commercial app in that run reported `has_preload=0`, so it is
  not valid allocator-replacement data.
- Setting `PROGRAM=app_process` and restarting framework makes zygote-related
  processes consume the malloc-debug path, but it is too broad for this test:
  package resolution failed for all target apps immediately afterward. Treat
  this as framework contamination, not as an app allocator stability result.
- Setting `PROGRAM=<package>` plus framework restart did not produce a clean
  commercial-app preload path in this session; PackageManager returned
  `No activity found` for third-party targets after the restart. A later adb
  wait observed a new boot id (`c16ef6eb-fd6c-458c-a73a-fd2dbc652ac8`), so this
  path also caused or coincided with a real reboot. After the reboot,
  `libc.debug.malloc.options` and `libc.debug.malloc.program` were empty again,
  `sys.boot_completed=1`, and Taobao launcher resolution recovered.

Current interpretation:

- The latest reboot seen in the global bg2 run is not attributable to LMKD. It
  happened while the old runner was pulling full maps from a large Taobao
  process, and the post-reboot `pstore` was empty.
- The largest remaining variable is not the SharedArena allocator itself, but
  the app injection path. Shell/native processes prove the adapter works;
  commercial apps are not yet being replaced consistently.
- Do not use LMKD kill-count comparison until `preload_checks.csv` proves that
  the same target app set has actually loaded `libc_malloc_debug.so` and
  `scudo_arena_` in both the narrow and multi-app scenarios.

## Run 6 - real wrapper path for Bilibili

Date: 2026-06-10

A Bilibili-only recovery helper was added:

```bash
tools/testing/memory_delegation/scripts/kernelsu/recover_phone_bili_scudo_wrapper.sh
```

It pushes:

```text
libscudo_md_preload.so
memory_delegation_broker
md_bili_scudo_wrap.sh
recover_bili_scudo_overlay_phone.sh
```

and configures only `wrap.tv.danmaku.bili`. The phone-side script now defaults
to a real wrapper path:

```text
wrap.tv.danmaku.bili=/data/local/tmp/md-bili-scudo/md_bili_scudo_wrap.sh
```

with `BIND_WRAP_TARGET=1` kept as a fallback to bind the same wrapper content to
`/system/bin/bugreportz`.

Direct wrapper-path smoke:

```bash
JUMP_HOST=lrc@192.168.61.230 BUILD=0 \
  tools/testing/memory_delegation/scripts/kernelsu/recover_phone_bili_scudo_wrapper.sh

python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --no-check-shared-arena-props \
  --rounds 1 \
  --force-stop-before-run \
  --stop-after-bili-start \
  --no-collect-memory \
  --out-dir /tmp/md-bili-wrapper-smoke
```

Output directory:

```text
/tmp/md-bili-wrapper-smoke/app-switch-20260610-124158
```

Result:

```text
wrap.tv.danmaku.bili=/data/local/tmp/md-bili-scudo/md_bili_scudo_wrap.sh
r1_bili_start: status=ok LaunchState=UNKNOWN WaitTime=3027ms
preload_checks.csv: has_preload=0 process=<not-running>
wrapper log: empty
boot_id unchanged
```

Fallback bind-path smoke:

```bash
JUMP_HOST=lrc@192.168.61.230 BUILD=0 BIND_WRAP_TARGET=1 \
  tools/testing/memory_delegation/scripts/kernelsu/recover_phone_bili_scudo_wrapper.sh

python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --no-check-shared-arena-props \
  --rounds 1 \
  --force-stop-before-run \
  --stop-after-bili-start \
  --no-collect-memory \
  --out-dir /tmp/md-bili-wrapper-bind-smoke
```

Output directory:

```text
/tmp/md-bili-wrapper-bind-smoke/app-switch-20260610-124250
```

Result:

```text
wrap.tv.danmaku.bili=/system/bin/bugreportz
r1_bili_start: status=ok LaunchState=UNKNOWN WaitTime=3014ms
preload_checks.csv: has_preload=0 process=<not-running>
wrapper log: empty
boot_id unchanged
```

Manual `monkey` launch did start Bilibili processes, but still did not invoke
the wrapper and did not map `libscudo_md_preload.so` or `scudo_arena_`.

Important device properties:

```text
ro.debuggable=0
wrap.tv.danmaku.bili=/system/bin/bugreportz
```

Current interpretation:

- The wrapper implementation is present and reproducible, but this production
  ROM appears to ignore `wrap.<pkg>` for the non-debuggable commercial app path.
- This explains why previous `wrap.<pkg>` runs could show the property as set
  while the target app maps still lacked preload/arena mappings.
- The next viable injection path likely needs a debuggable/test APK, a
  systemless module applied early enough to affect zygote policy, or a different
  framework-level hook. Continuing LMKD comparison with `wrap.<pkg>` on this ROM
  would still measure the default allocator for Bilibili.
- Standard ramoops/pstore did not preserve a kernel stack.
- The strongest available signal is the vendor minidump path reporting
  `panic = true`, but the minidump reader also reports size 0, so we still do
  not have a symbolic crash stack.

Next stability step:

- Re-run with the improved diagnostic wait logic so `dmesg`, `pstore`,
  `meminfo`, `ps`, and SharedArena logs are collected only after adb is stable,
  not during the transient `error: closed` window immediately after reboot.
- If pstore remains empty, isolate whether the reboot requires Bilibili itself
  or merely the app-switch preload wrapper by testing one wrapped package at a
  time.

## Run 5 - Bilibili wrapped launch isolation

Date: 2026-06-10

After Run 4 rebooted, the app-switch overlay was restored again. A narrower
runner mode was added:

```text
--stop-after-bili-start
```

Command:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --stop-after-bili-start \
  --collect-reboot-diagnostics \
  --out-dir /tmp/md-phone-stability-bili-only
```

Output directory:

```text
/tmp/md-phone-stability-bili-only/app-switch-20260610-114827
```

Result:

```text
r1_bili_start: status=ok LaunchState="UNKNOWN (0)" WaitTime=3022ms
lmkd_log_hits=0
```

Boot markers:

```text
pre_boot_id=b8ded189-7898-40ca-af76-999a102913d4
post_boot_id=b8ded189-7898-40ca-af76-999a102913d4
```

The wrapped Bilibili launcher start alone did not reboot the device. This
narrows the Run 4 failure away from the earliest wrapper/broker attach path and
toward the post-launch interaction path: scripted tap, Bilibili video/feed
native code, media pipeline, or a timing-sensitive state after launch.

Next isolation step:

- Run Bilibili-only with the video tap enabled but stop before launching
  QQ Music.
- If that reboots, inspect Bilibili/media related native logs and SharedArena
  traces around the tap.
- If that is stable, continue adding apps one by one until the first fatal
  reboot appears.

## Run 6 - Bilibili warm video tap isolation

Date: 2026-06-10

A second narrow runner mode was added:

```text
--stop-after-bili-video-tap
```

Command:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --stop-after-bili-video-tap \
  --collect-reboot-diagnostics \
  --out-dir /tmp/md-phone-stability-bili-tap
```

Output directory:

```text
/tmp/md-phone-stability-bili-tap/app-switch-20260610-114932
```

Result:

```text
r1_bili_start: status=ok LaunchState="UNKNOWN (0)" WaitTime=6ms TotalTime=0ms
lmkd_log_hits=0
```

This warm-start Bilibili tap path did not reboot the device.

## Run 7 - Bilibili cold video tap isolation

Date: 2026-06-10

Command:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --force-stop-before-run \
  --stop-after-bili-video-tap \
  --collect-reboot-diagnostics \
  --out-dir /tmp/md-phone-stability-bili-cold-tap
```

Output directory:

```text
/tmp/md-phone-stability-bili-cold-tap/app-switch-20260610-114956
```

Result:

```text
r1_bili_start: status=ok LaunchState=COLD WaitTime=1065ms TotalTime=1060ms
pre_boot_id=b8ded189-7898-40ca-af76-999a102913d4
post_boot_id=b8ded189-7898-40ca-af76-999a102913d4
```

This cold-start Bilibili tap path also did not reboot the device.

The run printed `lmkd_log_hits=48`, but inspection showed these were all
`lowmemorykiller: device memory ...` pressure status lines, not kill events. The
runner now writes both:

```text
lmkd.txt        # broad LMKD-related log lines
lmkd_kills.txt  # stricter kill-only subset
```

and prints both `lmkd_log_hits` and `lmkd_kill_hits`.

Current stability conclusion:

- The fatal reboot in Run 4 is real, but it is not reproduced by wrapped
  Bilibili launch alone, warm Bilibili tap, or cold Bilibili tap.
- The next likely trigger is later sequence state: returning Home after Bilibili,
  starting QQ Music with the same preload/broker state, or interaction between
  multiple wrapped app processes.

## Run 8 - Bilibili plus QQ Music start isolation

Date: 2026-06-10

A third narrow runner mode was added:

```text
--stop-after-qqmusic-start
```

Command:

```bash
python3 tools/testing/memory_delegation/scripts/run_phone_app_switch_sequence.py \
  --require-shared-arena-wrap \
  --rounds 1 \
  --force-stop-before-run \
  --stop-after-qqmusic-start \
  --collect-reboot-diagnostics \
  --out-dir /tmp/md-phone-stability-qqmusic-start
```

Output directory:

```text
/tmp/md-phone-stability-qqmusic-start/app-switch-20260610-115131
```

Result:

```text
r1_bili_start: status=ok LaunchState=COLD WaitTime=855ms TotalTime=849ms
r1_qqmusic_start: status=ok LaunchState=COLD WaitTime=540ms TotalTime=535ms
lmkd_log_hits=100
lmkd_kill_hits=0
```

This Bilibili + Home + QQ Music start boundary did not reboot the device and did
not contain strict LMKD kill events. The broad LMKD count again mostly reflects
pressure status logs.

Updated stability conclusion:

- Run 4 remains a real fatal reboot sample, but the trigger is not reproduced by
  Bilibili launch/tap or by adding QQ Music launcher start.
- The remaining likely trigger points are QQ Music media interaction, the first
  background app after QQ Music, or cumulative multi-wrapped-app state.
