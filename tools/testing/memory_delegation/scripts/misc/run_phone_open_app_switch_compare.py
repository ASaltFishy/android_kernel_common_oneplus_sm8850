#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Compare baseline vs memory-delegation probe builds with two open-source apps:
# AntennaPod and VLC. Both APKs contain the probe, but it only loads when the
# app-private file files/enable_memory_delegation_probe exists.

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import re
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path


LMKD_KILL_RE = re.compile(
    r"(?:\bam_kill\b|kill_one_process|Killing [' ].*pid|Killing .*adj|"
    r"lowmemorykiller: Killing|kill(ed)? .*uid)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class App:
    name: str
    package: str
    apk: str


APPS = [
    App(
        "AntennaPod",
        "de.danoeh.antennapod.debug",
        "/home/lrc/patent/apps/AntennaPod/app/build/outputs/apk/free/debug/app-free-debug.apk",
    ),
    App(
        "VLC",
        "org.videolan.vlc.debug",
        "/home/lrc/patent/apps/vlc-android/application/app/build/outputs/apk/debug/VLC-Android-3.7.1-debug-all.apk",
    ),
]


class Adb:
    def __init__(self, adb_bin: str, serial: str):
        self.adb_bin = adb_bin
        self.serial = serial

    def cmd(self, args: list[str]) -> list[str]:
        out = [self.adb_bin]
        if self.serial:
            out += ["-s", self.serial]
        return out + args

    def run(self, args: list[str], check: bool = True, timeout: int = 60) -> str:
        cmd = self.cmd(args)
        print("+ " + shlex.join(cmd), flush=True)
        proc = subprocess.run(
            cmd,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
        if check and proc.returncode:
            raise RuntimeError(f"command failed ({proc.returncode}): {shlex.join(cmd)}\n{proc.stdout}")
        return proc.stdout.replace("\r\n", "\n")

    def shell(self, command: str, check: bool = True, timeout: int = 60) -> str:
        return self.run(["shell", command], check=check, timeout=timeout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run AntennaPod/VLC baseline vs memory-delegation app switch comparison.")
    parser.add_argument("--adb-bin", default=os.environ.get("ADB_BIN", "adb"))
    parser.add_argument("--serial", default=os.environ.get("ADB_SERIAL", "3B15AL00K5D00000"))
    parser.add_argument("--out-dir", default=os.environ.get("OUT_DIR", "/tmp/md-open-app-switch"))
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--settle-s", type=float, default=4.0)
    parser.add_argument("--install", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--variants", nargs="+", choices=["baseline", "md"], default=["baseline", "md"])
    parser.add_argument("--broker", default="/home/lrc/patent/kernel/kernel_platform/common/tools/testing/memory_delegation/tests/out/memory_delegation_broker")
    parser.add_argument("--logcat-lines", type=int, default=8000)
    return parser.parse_args()


def resolve_activity(adb: Adb, package: str) -> str:
    raw = adb.shell(
        "cmd package resolve-activity --brief "
        "-a android.intent.action.MAIN -c android.intent.category.LAUNCHER "
        f"-p {shlex.quote(package)} 2>/dev/null | tail -n 1",
        check=False,
        timeout=20,
    ).strip()
    if "/" not in raw:
        raise RuntimeError(f"cannot resolve launcher for {package}: {raw}")
    return raw.splitlines()[-1].strip()


def pidof(adb: Adb, package: str) -> list[int]:
    raw = adb.shell(f"pidof {shlex.quote(package)}", check=False, timeout=10).strip()
    return [int(x) for x in raw.split() if x.isdigit()]


def parse_kb(raw: str, key: str) -> int:
    match = re.search(rf"^{re.escape(key)}:\s+(\d+)\s+kB", raw, re.MULTILINE)
    return int(match.group(1)) if match else 0


def read_proc_file(adb: Adb, pid: int, name: str, timeout: int = 20) -> str:
    return adb.shell(f"su -c 'cat /proc/{pid}/{name} 2>/dev/null'", check=False, timeout=timeout)


def arena_stats_from_smaps(smaps: str) -> tuple[int, int, int]:
    arena_count = 0
    arena_pss = 0
    secondary_pss = 0
    current = ""
    for line in smaps.splitlines():
        if re.match(r"^[0-9a-f]+-[0-9a-f]+", line):
            current = line
            if "scudo_arena_" in line:
                arena_count += 1
        elif line.startswith("Pss:"):
            value = int(line.split()[1])
            if "scudo_arena_" in current:
                arena_pss += value
            elif "scudo:secondary" in current:
                secondary_pss += value
    return arena_count, arena_pss, secondary_pss


def collect_app_sample(adb: Adb, variant: str, step: str, app: App) -> dict[str, object]:
    pids = pidof(adb, app.package)
    row: dict[str, object] = {
        "variant": variant,
        "step": step,
        "app": app.name,
        "package": app.package,
        "pids": " ".join(str(p) for p in pids),
        "pid_count": len(pids),
        "rss_kb": 0,
        "pss_kb": 0,
        "private_dirty_kb": 0,
        "swap_pss_kb": 0,
        "arena_map_count": 0,
        "arena_pss_kb": 0,
        "secondary_pss_kb": 0,
        "marker": "",
    }
    for pid in pids:
        rollup = read_proc_file(adb, pid, "smaps_rollup")
        row["rss_kb"] = int(row["rss_kb"]) + parse_kb(rollup, "Rss")
        row["pss_kb"] = int(row["pss_kb"]) + parse_kb(rollup, "Pss")
        row["private_dirty_kb"] = int(row["private_dirty_kb"]) + parse_kb(rollup, "Private_Dirty")
        row["swap_pss_kb"] = int(row["swap_pss_kb"]) + parse_kb(rollup, "SwapPss")
        smaps = read_proc_file(adb, pid, "smaps", timeout=45)
        arena_count, arena_pss, secondary_pss = arena_stats_from_smaps(smaps)
        row["arena_map_count"] = int(row["arena_map_count"]) + arena_count
        row["arena_pss_kb"] = int(row["arena_pss_kb"]) + arena_pss
        row["secondary_pss_kb"] = int(row["secondary_pss_kb"]) + secondary_pss
    marker = adb.shell(
        f"run-as {shlex.quote(app.package)} cat files/memory_delegation_loader.txt 2>/dev/null",
        check=False,
        timeout=10,
    ).strip()
    row["marker"] = marker.replace("\n", " | ")
    return row


def collect_system_sample(adb: Adb, variant: str, step: str) -> dict[str, object]:
    meminfo = adb.shell("cat /proc/meminfo", check=False, timeout=20)
    return {
        "variant": variant,
        "step": step,
        "mem_available_kb": parse_kb(meminfo, "MemAvailable"),
        "mem_free_kb": parse_kb(meminfo, "MemFree"),
        "cached_kb": parse_kb(meminfo, "Cached"),
        "swap_free_kb": parse_kb(meminfo, "SwapFree"),
        "anon_pages_kb": parse_kb(meminfo, "AnonPages"),
    }


def install_apps(adb: Adb) -> None:
    for app in APPS:
        adb.run(["push", app.apk, f"/data/local/tmp/{Path(app.apk).name}"], timeout=120)
        adb.shell(f"pm install -r -d /data/local/tmp/{shlex.quote(Path(app.apk).name)}", timeout=120)


def set_probe_enabled(adb: Adb, enabled: bool) -> None:
    for app in APPS:
        adb.shell(f"am force-stop {shlex.quote(app.package)}", check=False, timeout=15)
        adb.shell(
            f"run-as {shlex.quote(app.package)} sh -c "
            f"{shlex.quote('mkdir -p files; rm -f files/memory_delegation_loader.txt files/enable_memory_delegation_probe')}",
            check=False,
            timeout=15,
        )
        if enabled:
            adb.shell(
                f"run-as {shlex.quote(app.package)} sh -c "
                f"{shlex.quote('mkdir -p files; touch files/enable_memory_delegation_probe')}",
                check=False,
                timeout=15,
            )


def start_broker(adb: Adb, broker: str, variant_dir: Path) -> None:
    adb.shell(
        "su -c 'for p in $(pidof memory_delegation_broker 2>/dev/null); do kill -KILL \"$p\" 2>/dev/null || true; done; "
        "rm -rf /data/local/tmp/md-open-app-switch; mkdir -p /data/local/tmp/md-open-app-switch; chmod 777 /data/local/tmp/md-open-app-switch'",
        check=False,
        timeout=20,
    )
    adb.run(["push", broker, "/data/local/tmp/md-open-app-switch/memory_delegation_broker"], timeout=60)
    adb.shell(
        "su -c 'chmod 755 /data/local/tmp/md-open-app-switch/memory_delegation_broker; "
        "cd /data/local/tmp/md-open-app-switch; ./memory_delegation_broker --trace --ashmem > broker.log 2>&1 &'",
        check=False,
        timeout=20,
    )
    time.sleep(1)
    (variant_dir / "broker_pid.txt").write_text(
        adb.shell("pidof memory_delegation_broker", check=False, timeout=10),
        encoding="utf-8",
    )


def stop_broker(adb: Adb) -> None:
    adb.shell(
        "su -c 'for p in $(pidof memory_delegation_broker 2>/dev/null); do kill -KILL \"$p\" 2>/dev/null || true; done'",
        check=False,
        timeout=20,
    )


def start_app(adb: Adb, component: str) -> str:
    return adb.shell(f"am start -W -n {shlex.quote(component)}", check=False, timeout=60)


def collect_lmkd(adb: Adb, variant_dir: Path, lines: int) -> int:
    raw = adb.run(["logcat", "-d", "-v", "time", "-t", str(lines)], check=False, timeout=90)
    (variant_dir / "logcat.txt").write_text(raw, encoding="utf-8", errors="replace")
    kills = [line for line in raw.splitlines() if LMKD_KILL_RE.search(line)]
    (variant_dir / "lmkd_kills.txt").write_text("\n".join(kills) + ("\n" if kills else ""), encoding="utf-8")
    return len(kills)


def run_variant(adb: Adb, args: argparse.Namespace, root: Path, variant: str, components: dict[str, str]) -> None:
    variant_dir = root / variant
    variant_dir.mkdir(parents=True, exist_ok=True)
    enabled = variant == "md"
    print(f"== variant {variant} enabled={enabled} ==", flush=True)

    set_probe_enabled(adb, enabled)
    if enabled:
        start_broker(adb, args.broker, variant_dir)
    else:
        stop_broker(adb)

    adb.run(["logcat", "-c"], check=False, timeout=30)
    app_rows: list[dict[str, object]] = []
    system_rows: list[dict[str, object]] = []
    launch_rows: list[dict[str, object]] = []

    system_rows.append(collect_system_sample(adb, variant, "before"))
    for i in range(args.rounds):
        for app in APPS:
            step = f"r{i + 1}_{app.name.lower()}_fg"
            raw = start_app(adb, components[app.package])
            (variant_dir / f"{step}.am_start.txt").write_text(raw, encoding="utf-8", errors="replace")
            launch_rows.append({
                "variant": variant,
                "step": step,
                "app": app.name,
                "wait_time_ms": parse_am_time(raw, "WaitTime"),
                "total_time_ms": parse_am_time(raw, "TotalTime"),
                "launch_state": parse_am_field(raw, "LaunchState"),
            })
            time.sleep(args.settle_s)
            system_rows.append(collect_system_sample(adb, variant, step))
            for sample_app in APPS:
                app_rows.append(collect_app_sample(adb, variant, step, sample_app))
            adb.shell("input keyevent KEYCODE_HOME", check=False, timeout=10)
            time.sleep(1)

    lmkd_kills = collect_lmkd(adb, variant_dir, args.logcat_lines)
    (variant_dir / "summary.txt").write_text(
        f"variant={variant}\nrounds={args.rounds}\nlmkd_kill_hits={lmkd_kills}\n",
        encoding="utf-8",
    )
    write_csv(variant_dir / "app_memory.csv", app_rows)
    write_csv(variant_dir / "system_memory.csv", system_rows)
    write_csv(variant_dir / "launch_times.csv", launch_rows)
    if enabled:
        broker_log = adb.shell(
            "su -c 'cat /data/local/tmp/md-open-app-switch/broker.log 2>/dev/null'",
            check=False,
            timeout=20,
        )
        (variant_dir / "broker.log").write_text(broker_log, encoding="utf-8", errors="replace")
    stop_broker(adb)


def parse_am_field(raw: str, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}:\s*(.*)$", raw, re.MULTILINE)
    return match.group(1).strip() if match else ""


def parse_am_time(raw: str, name: str) -> int:
    value = parse_am_field(raw, name)
    try:
        return int(value)
    except ValueError:
        return 0


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    keys: list[str] = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    os.environ.setdefault("ADB_SERVER_SOCKET", "tcp:127.0.0.1:15037")
    root = Path(args.out_dir) / f"open-app-switch-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}"
    root.mkdir(parents=True, exist_ok=True)
    adb = Adb(args.adb_bin, args.serial)
    adb.run(["get-state"], timeout=20)
    if args.install:
        install_apps(adb)
    components = {app.package: resolve_activity(adb, app.package) for app in APPS}
    (root / "components.txt").write_text(
        "\n".join(f"{pkg} {component}" for pkg, component in components.items()) + "\n",
        encoding="utf-8",
    )
    for variant in args.variants:
        run_variant(adb, args, root, variant, components)
    print(f"out_dir={root}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
