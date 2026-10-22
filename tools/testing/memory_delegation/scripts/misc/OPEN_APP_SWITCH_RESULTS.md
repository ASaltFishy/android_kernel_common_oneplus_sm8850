# Open-source app switch results

Date: 2026-06-11

## Scope

This experiment compares two open-source Android apps:

- AntennaPod: `de.danoeh.antennapod.debug`
- VLC Android: `org.videolan.vlc.debug`

Both APKs contain the memory-delegation probe libraries, but the probe only
loads when the app-private file `files/enable_memory_delegation_probe` exists.
This lets the same APK run in two modes:

- `baseline`: probe disabled, no broker.
- `md`: probe enabled, `memory_delegation_broker --trace --ashmem` running.

## Command

```bash
export ADB_SERVER_SOCKET=tcp:127.0.0.1:15037
python3 tools/testing/memory_delegation/scripts/run_phone_open_app_switch_compare.py \
  --rounds 6 \
  --settle-s 4 \
  --out-dir /tmp/md-open-app-switch
```

Output directory:

```text
/tmp/md-open-app-switch/open-app-switch-20260611-182049
```

The runner installs the current AntennaPod/VLC debug APKs, resolves launcher
activities, clears logcat per variant, force-stops both apps before each
variant, and records:

- `app_memory.csv`: per-app RSS/PSS/private dirty/swap PSS from
  `/proc/<pid>/smaps_rollup`.
- `arena_pss_kb`, `arena_map_count`, `secondary_pss_kb` from `/proc/<pid>/smaps`.
- app-private probe marker from `files/memory_delegation_loader.txt`.
- `launch_times.csv` from `am start -W`.
- strict LMKD kill lines in `lmkd_kills.txt`.
- broker log in the `md` variant.

## Result Summary

| Variant | App | Samples | Last PSS MB | Peak PSS MB | Last RSS MB | Peak RSS MB | Last arena maps | Peak arena PSS MB | LMKD kills |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| baseline | AntennaPod | 12 | 101.5 | 113.4 | 246.0 | 253.4 | 0 | 0.0 | 0 |
| baseline | VLC | 11 | 132.1 | 133.7 | 278.3 | 279.9 | 0 | 0.0 | 0 |
| md | AntennaPod | 12 | 101.8 | 113.5 | 246.5 | 253.6 | 8 | 0.0 | 0 |
| md | VLC | 11 | 132.4 | 134.5 | 278.8 | 280.4 | 8 | 0.0 | 0 |

Launch latency was also effectively unchanged:

| Variant | App | Avg WaitTime ms | Max WaitTime ms |
| --- | --- | ---: | ---: |
| baseline | AntennaPod | 144.0 | 619 |
| baseline | VLC | 129.7 | 492 |
| md | AntennaPod | 143.8 | 607 |
| md | VLC | 129.7 | 495 |

System memory:

| Variant | MemAvailable first MB | MemAvailable last MB | MemAvailable min MB |
| --- | ---: | ---: | ---: |
| baseline | 4499.3 | 4298.4 | 4227.6 |
| md | 4505.4 | 4129.7 | 4129.7 |

## Interpretation

The run is stable: no adb disconnect, no reboot, and no strict LMKD kill event.

The memory-delegation attach path is working in both apps. In the `md` variant
both apps reported successful native smoke checks and 8 `scudo_arena_*`
mappings.

This result should not yet be interpreted as real application memory savings.
The measured PSS/RSS difference is within noise for this workload, and
`arena_pss_kb` is 0 at steady-state. That means the current integration proves
that the probe can attach to the broker, but the ordinary AntennaPod/VLC UI
switch workload is not yet producing persistent real allocations through the
shared arena.

## Next Step

To make this a meaningful allocator comparison, the app workload must keep real
native allocations in the modified allocator path. Practical next options:

- Add an app-controlled native stress action to each APK and trigger it during
  app switching.
- For VLC, move beyond the small probe and make a selected native media path or
  JNI-side workload link/use the modified allocator.
- Add a memory pressure sidecar after both apps are backgrounded if the goal is
  specifically to observe LMKD kill behavior. With only these two apps and the
  current light workload, LMKD is not expected to trigger.
