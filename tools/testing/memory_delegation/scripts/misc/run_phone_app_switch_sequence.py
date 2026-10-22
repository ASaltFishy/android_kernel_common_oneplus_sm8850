#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Replay a multi-application foreground/background sequence and collect
# ActivityManager launch/switch latency from `am start -W`.

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_APPS = {
    "bili": ("哔哩哔哩", "tv.danmaku.bili"),
    "qqmusic": ("QQ音乐", "com.tencent.qqmusic"),
    "taobao": ("淘宝", "com.taobao.taobao"),
    "qqlive": ("腾讯视频", "com.tencent.qqlive"),
    "alibaba": ("阿里巴巴", "com.alibaba.wireless"),
    "baidu": ("百度", "com.baidu.searchbox"),
    "tomato": ("番茄免费小说", "com.dragon.read"),
    "amap": ("高德地图", "com.autonavi.minimap"),
    "weibo": ("微博", "com.sina.weibo"),
    "xhs": ("小红书", "com.xingin.xhs"),
    "douyin": ("抖音", "com.ss.android.ugc.aweme"),
}


LMKD_RE = re.compile(
    r"(?:\blmkd\b|lowmemorykiller|Killing .*adj|kill(ed)? .*uid|am_kill)",
    re.IGNORECASE,
)

LMKD_KILL_RE = re.compile(
    r"(?:\bam_kill\b|kill_one_process|Killing [' ].*pid|Killing .*adj|"
    r"lowmemorykiller: Killing|kill(ed)? .*uid)",
    re.IGNORECASE,
)


@dataclass
class App:
    key: str
    name: str
    package: str
    component: str | None = None


@dataclass
class LaunchResult:
    label: str
    app_name: str
    package: str
    component: str
    status: str
    launch_state: str
    total_time_ms: int | None
    wait_time_ms: int | None
    raw: str


@dataclass
class PreloadCheck:
    label: str
    package: str
    pid: int
    process: str
    has_preload: bool
    maps_match: str
    check_error: str = ""


class Adb:
    def __init__(self, adb_bin: str, serial: str, jump_host: str | None, dry_run: bool):
        self.adb_bin = adb_bin
        self.serial = serial
        self.jump_host = jump_host
        self.dry_run = dry_run

    def _adb_cmd(self) -> list[str]:
        cmd = [self.adb_bin]
        if self.serial:
            cmd += ["-s", self.serial]
        return cmd

    def run(self, args: list[str], check: bool = True, timeout: int = 60) -> str:
        adb_cmd = self._adb_cmd() + args
        cmd = ["ssh", self.jump_host, shlex.join(adb_cmd)] if self.jump_host else adb_cmd
        print("+ " + shlex.join(cmd), flush=True)
        if self.dry_run:
            return ""
        proc = subprocess.run(
            cmd,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        if check and proc.returncode != 0:
            raise RuntimeError(f"command failed ({proc.returncode}): {shlex.join(cmd)}\n{proc.stdout}")
        return proc.stdout.replace("\r\n", "\n")

    def shell(self, command: str, check: bool = True, timeout: int = 60) -> str:
        return self.run(["shell", command], check=check, timeout=timeout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the multi-app Bili/QQMusic/background sequence and measure Xiaohongshu -> Douyin switch latency."
    )
    parser.add_argument("--jump-host", default=os.environ.get("JUMP_HOST", ""))
    parser.add_argument("--adb-bin", default=os.environ.get("ADB_BIN", "adb"))
    parser.add_argument("--serial", default=os.environ.get("ADB_SERIAL", "3B15AL00K5D00000"))
    parser.add_argument("--out-dir", default=os.environ.get("OUT_DIR", "/tmp/md-phone-run"))
    parser.add_argument("--sleep-after-start", type=float, default=3.0)
    parser.add_argument("--sleep-after-home", type=float, default=1.0)
    parser.add_argument("--bili-video-tap-x", type=float, default=float(os.environ.get("BILI_VIDEO_TAP_X", "0.25")))
    parser.add_argument("--bili-video-tap-y", type=float, default=float(os.environ.get("BILI_VIDEO_TAP_Y", "0.34")))
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--clear-logcat", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--force-stop-before-run", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--check-shared-arena-props", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--require-shared-arena-wrap", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--expected-wrap", default=os.environ.get("EXPECTED_WRAP", "/system/bin/bugreportz"))
    parser.add_argument("--logcat-lines", type=int, default=int(os.environ.get("LOGCAT_LINES", "8000")))
    parser.add_argument("--diagnostic-logcat-lines", type=int, default=int(os.environ.get("DIAGNOSTIC_LOGCAT_LINES", "20000")))
    parser.add_argument("--reboot-wait-timeout", type=int, default=int(os.environ.get("REBOOT_WAIT_TIMEOUT", "180")))
    parser.add_argument("--collect-memory", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--collect-reboot-diagnostics", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--stop-after-bili-start", action="store_true")
    parser.add_argument("--stop-after-bili-video-tap", action="store_true")
    parser.add_argument("--stop-after-qqmusic-start", action="store_true")
    parser.add_argument("--stop-after-qqmusic-media", action="store_true")
    parser.add_argument("--max-background-apps", type=int, default=-1)
    parser.add_argument("--resolve-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    for key, (_, pkg) in DEFAULT_APPS.items():
        parser.add_argument(f"--pkg-{key}", default=os.environ.get(f"PKG_{key.upper()}", pkg))
    return parser.parse_args()


def parse_wm_size(out: str) -> tuple[int, int]:
    match = re.search(r"Physical size:\s*(\d+)x(\d+)", out)
    if not match:
        match = re.search(r"Override size:\s*(\d+)x(\d+)", out)
    if not match:
        return (1080, 2400)
    return (int(match.group(1)), int(match.group(2)))


def tap_frac(adb: Adb, width: int, height: int, x_frac: float, y_frac: float) -> None:
    x = max(1, min(width - 1, round(width * x_frac)))
    y = max(1, min(height - 1, round(height * y_frac)))
    adb.shell(f"input tap {x} {y}", check=False, timeout=10)


def home(adb: Adb, sleep_secs: float) -> None:
    adb.shell("input keyevent KEYCODE_HOME", check=False, timeout=10)
    time.sleep(sleep_secs)


def media_play(adb: Adb) -> None:
    adb.shell("input keyevent KEYCODE_MEDIA_PLAY", check=False, timeout=10)


def resolve_activity(adb: Adb, app: App) -> str | None:
    cmd = (
        "cmd package resolve-activity --brief "
        "-a android.intent.action.MAIN -c android.intent.category.LAUNCHER "
        f"-p {shlex.quote(app.package)} 2>/dev/null | tail -n 1"
    )
    out = adb.shell(cmd, check=False, timeout=20).strip()
    if not out or "No activity found" in out or "/" not in out:
        out = adb.shell(f"cmd package resolve-activity --brief {shlex.quote(app.package)} 2>/dev/null | tail -n 1",
                        check=False, timeout=20).strip()
    if not out or "No activity found" in out or "/" not in out:
        return None
    return out.splitlines()[-1].strip()


def parse_launch(label: str, app: App, component: str, raw: str) -> LaunchResult:
    def field(name: str) -> str:
        match = re.search(rf"^{re.escape(name)}:\s*(.*)$", raw, re.MULTILINE)
        return match.group(1).strip() if match else ""

    def int_field(name: str) -> int | None:
        value = field(name)
        try:
            return int(value)
        except ValueError:
            return None

    return LaunchResult(
        label=label,
        app_name=app.name,
        package=app.package,
        component=component,
        status=field("Status"),
        launch_state=field("LaunchState"),
        total_time_ms=int_field("TotalTime"),
        wait_time_ms=int_field("WaitTime"),
        raw=raw,
    )


def start_app(adb: Adb, app: App, label: str) -> LaunchResult:
    if not app.component:
        raise RuntimeError(f"{app.name} ({app.package}) has no resolved launcher activity")
    raw = adb.shell(f"am start -W -n {shlex.quote(app.component)}", check=False, timeout=45)
    result = parse_launch(label, app, app.component, raw)
    print(
        f"[{label}] {app.name}: status={result.status or '-'} "
        f"state={result.launch_state or '-'} wait={result.wait_time_ms} total={result.total_time_ms}",
        flush=True,
    )
    if result.wait_time_ms is None:
        preview = " | ".join(line.strip() for line in raw.splitlines()[:6])
        print(f"[{label}] WARNING: cannot parse am start output: {preview}", flush=True)
    return result


def parse_ps_for_package(raw: str, package: str) -> list[tuple[int, str]]:
    procs: list[tuple[int, str]] = []
    for line in raw.splitlines():
        cols = line.split()
        if len(cols) < 9 or not cols[1].isdigit():
            continue
        name = cols[-1]
        if name == package or name.startswith(package + ":"):
            procs.append((int(cols[1]), name))
    return procs


def collect_preload_check(adb: Adb, app: App, label: str, out_dir: Path) -> list[PreloadCheck]:
    ps_raw = adb.shell("ps -A", check=False, timeout=30)
    procs = parse_ps_for_package(ps_raw, app.package)
    checks: list[PreloadCheck] = []
    detail_dir = out_dir / "preload-checks"
    detail_dir.mkdir(parents=True, exist_ok=True)

    for pid, proc_name in procs:
        safe_proc = proc_name.replace(":", "_").replace("/", "_")
        maps_cmd = (
            "grep -E 'libaconfig_test_rust_library\\.dylib\\.so|"
            "libscudo_md_preload\\.so|libc_malloc_debug\\.so|scudo_arena_' "
            f"/proc/{pid}/maps 2>/dev/null | head -40"
        )
        check_error = ""
        try:
            maps = adb.shell(
                f"su -c {shlex.quote(maps_cmd)} 2>/dev/null || {maps_cmd}",
                check=False,
                timeout=5,
            )
        except subprocess.TimeoutExpired as exc:
            maps = ""
            check_error = f"timeout after {exc.timeout}s"
        except Exception as exc:
            maps = ""
            check_error = f"{type(exc).__name__}: {exc}"
        (detail_dir / f"{label}_{pid}_{safe_proc}.maps.txt").write_text(
            maps + (f"\nCHECK_ERROR: {check_error}\n" if check_error else ""),
            encoding="utf-8",
            errors="replace",
        )
        matches = [
            line for line in maps.splitlines()
            if "libaconfig_test_rust_library.dylib.so" in line
            or "libscudo_md_preload.so" in line
            or "libc_malloc_debug.so" in line
            or "scudo_arena_" in line
        ]
        checks.append(PreloadCheck(
            label=label,
            package=app.package,
            pid=pid,
            process=proc_name,
            has_preload=bool(matches),
            maps_match=" | ".join(matches[:4]),
            check_error=check_error,
        ))

    if not procs:
        checks.append(PreloadCheck(
            label=label,
            package=app.package,
            pid=0,
            process="<not-running>",
            has_preload=False,
            maps_match="",
            check_error="",
        ))

    for check in checks:
        print(
            f"[{label}] preload pid={check.pid} process={check.process} "
            f"has_preload={int(check.has_preload)}"
            + (f" check_error={check.check_error}" if check.check_error else ""),
            flush=True,
        )
    return checks


def collect_lmkd(adb: Adb, out_dir: Path, lines: int) -> tuple[int, int]:
    raw = adb.run(["logcat", "-d", "-v", "time", "-t", str(lines)], check=False, timeout=90)
    log_path = out_dir / "logcat.txt"
    log_path.write_text(raw, encoding="utf-8", errors="replace")
    hits = [line for line in raw.splitlines() if LMKD_RE.search(line)]
    kill_hits = [line for line in hits if LMKD_KILL_RE.search(line)]
    (out_dir / "lmkd.txt").write_text("\n".join(hits) + ("\n" if hits else ""), encoding="utf-8")
    (out_dir / "lmkd_kills.txt").write_text(
        "\n".join(kill_hits) + ("\n" if kill_hits else ""), encoding="utf-8"
    )
    return len(hits), len(kill_hits)


def collect_boot_marker(adb: Adb, out_dir: Path, label: str) -> str:
    raw = read_boot_marker(adb)
    (out_dir / f"{label}_boot_marker.txt").write_text(raw, encoding="utf-8", errors="replace")
    return raw


def read_boot_marker(adb: Adb) -> str:
    return adb.shell(
        "printf 'date='; date; "
        "printf 'boot_id='; cat /proc/sys/kernel/random/boot_id 2>/dev/null; "
        "printf 'uptime='; cat /proc/uptime 2>/dev/null; "
        "printf 'sys.boot_completed='; getprop sys.boot_completed; "
        "printf 'sys.boot.reason='; getprop sys.boot.reason; "
        "printf 'ro.boot.bootreason='; getprop ro.boot.bootreason",
        check=False,
        timeout=20,
    )


def boot_id_from_marker(raw: str) -> str:
    match = re.search(r"^boot_id=(.*)$", raw, re.MULTILINE)
    return match.group(1).strip() if match else ""


def wait_for_stable_adb(adb: Adb, timeout: int) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        try:
            last = read_boot_marker(adb)
            if boot_id_from_marker(last) and "error: closed" not in last.lower():
                return last
        except Exception as exc:
            last = f"{type(exc).__name__}: {exc}"
        time.sleep(5)
    return last


def write_shell_capture(adb: Adb, path: Path, command: str, timeout: int = 60) -> None:
    try:
        raw = adb.shell(command, check=False, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raw = f"TIMEOUT after {exc.timeout}s: {command}\n"
    except Exception as exc:
        raw = f"ERROR while running {command}: {exc}\n"
    path.write_text(raw, encoding="utf-8", errors="replace")


def collect_reboot_diagnostics(
    adb: Adb,
    out_dir: Path,
    args: argparse.Namespace,
    label: str,
    pre_boot_marker: str,
) -> None:
    if args.dry_run:
        return

    diag_dir = out_dir / f"reboot-diagnostics-{label}"
    diag_dir.mkdir(parents=True, exist_ok=True)
    summary = diag_dir / "summary.txt"
    lines = [f"label={label}", f"started={dt.datetime.now().isoformat()}"]

    try:
        adb.run(["wait-for-device"], check=False, timeout=args.reboot_wait_timeout)
        lines.append("wait_for_device=ok")
    except Exception as exc:
        lines.append(f"wait_for_device=failed: {exc}")

    post_marker = wait_for_stable_adb(adb, args.reboot_wait_timeout)
    (diag_dir / "post_boot_marker.txt").write_text(
        post_marker, encoding="utf-8", errors="replace"
    )
    if not boot_id_from_marker(post_marker):
        lines.append("post_boot_marker=unavailable")

    pre_boot_id = boot_id_from_marker(pre_boot_marker)
    post_boot_id = boot_id_from_marker(post_marker)
    lines.append(f"pre_boot_id={pre_boot_id or '<unknown>'}")
    lines.append(f"post_boot_id={post_boot_id or '<unknown>'}")
    if pre_boot_id and post_boot_id:
        lines.append(f"boot_id_changed={int(pre_boot_id != post_boot_id)}")

    write_shell_capture(adb, diag_dir / "getprop.txt", "getprop", timeout=30)
    write_shell_capture(adb, diag_dir / "proc_meminfo.txt", "cat /proc/meminfo", timeout=20)
    write_shell_capture(adb, diag_dir / "proc_vmstat.txt", "cat /proc/vmstat", timeout=20)
    write_shell_capture(adb, diag_dir / "pressure_memory.txt", "cat /proc/pressure/memory 2>/dev/null", timeout=20)
    write_shell_capture(adb, diag_dir / "ps_A.txt", "ps -A", timeout=30)
    write_shell_capture(
        adb,
        diag_dir / "dmesg.txt",
        "su -c 'dmesg' 2>/dev/null || dmesg 2>&1",
        timeout=60,
    )
    write_shell_capture(
        adb,
        diag_dir / "pstore.txt",
        "su -c '"
        "ls -la /sys/fs/pstore 2>/dev/null; "
        "for f in /sys/fs/pstore/*; do "
        "[ -e \"$f\" ] || continue; "
        "echo ==== $f ====; "
        "cat \"$f\" 2>/dev/null; "
        "done"
        "'",
        timeout=60,
    )
    write_shell_capture(
        adb,
        diag_dir / "tombstones.txt",
        "su -c '"
        "ls -lt /data/tombstones 2>/dev/null | head -40; "
        "for f in $(ls -t /data/tombstones/tombstone_* 2>/dev/null | head -5); do "
        "echo ==== $f ====; "
        "sed -n \"1,220p\" \"$f\" 2>/dev/null; "
        "done"
        "'",
        timeout=60,
    )
    write_shell_capture(
        adb,
        diag_dir / "shared_arena_logs.txt",
        "su -c '"
        "for f in "
        "/data/local/tmp/md-bili-scudo/app_switch_broker.log "
        "/data/local/tmp/md_app_switch_scudo_trace.log "
        "/data/local/tmp/md_app_switch_scudo_wrap.log "
        "/data/local/tmp/md/broker.log; do "
        "echo ==== $f ====; "
        "ls -l \"$f\" 2>/dev/null; "
        "tail -n 240 \"$f\" 2>/dev/null; "
        "done"
        "'",
        timeout=60,
    )

    try:
        raw = adb.run(
            ["logcat", "-d", "-b", "all", "-v", "threadtime", "-t", str(args.diagnostic_logcat_lines)],
            check=False,
            timeout=120,
        )
    except subprocess.TimeoutExpired as exc:
        raw = f"TIMEOUT after {exc.timeout}s while collecting logcat\n"
    except Exception as exc:
        raw = f"ERROR while collecting logcat: {exc}\n"
    (diag_dir / "logcat_all.txt").write_text(raw, encoding="utf-8", errors="replace")

    interesting = re.compile(
        r"(Kernel panic|Oops|BUG:|Unable to handle|watchdog|subsys|fatal|tombstone|"
        r"system_server|zygote|memory_delegation|shared_arena|scudo|lmkd|lowmemorykiller|"
        r"reboot|userspace reboot)",
        re.IGNORECASE,
    )
    hits = [line for line in raw.splitlines() if interesting.search(line)]
    (diag_dir / "logcat_interesting.txt").write_text(
        "\n".join(hits) + ("\n" if hits else ""), encoding="utf-8", errors="replace"
    )

    lines.append(f"diagnostic_dir={diag_dir}")
    summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"reboot_diagnostic_dir={diag_dir}", flush=True)


def write_results(out_dir: Path, results: list[LaunchResult]) -> None:
    with (out_dir / "launch_times.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "label",
                "app_name",
                "package",
                "component",
                "status",
                "launch_state",
                "total_time_ms",
                "wait_time_ms",
            ],
        )
        writer.writeheader()
        for r in results:
            writer.writerow({
                "label": r.label,
                "app_name": r.app_name,
                "package": r.package,
                "component": r.component,
                "status": r.status,
                "launch_state": r.launch_state,
                "total_time_ms": r.total_time_ms,
                "wait_time_ms": r.wait_time_ms,
            })
    with (out_dir / "am_start_raw.txt").open("w", encoding="utf-8", errors="replace") as f:
        for r in results:
            f.write(f"===== {r.label} {r.app_name} {r.package} =====\n")
            f.write(r.raw)
            if not r.raw.endswith("\n"):
                f.write("\n")


def write_preload_checks(out_dir: Path, checks: list[PreloadCheck]) -> None:
    with (out_dir / "preload_checks.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "label",
                "package",
                "pid",
                "process",
                "has_preload",
                "maps_match",
                "check_error",
            ],
        )
        writer.writeheader()
        for check in checks:
            writer.writerow({
                **check.__dict__,
                "has_preload": int(check.has_preload),
            })


def collect_memory_snapshot(args: argparse.Namespace, out_dir: Path, snapshot_name: str) -> None:
    script = Path(__file__).with_name("collect_phone_app_memory.py")
    if not script.exists():
        print(f"WARNING: memory snapshot script not found: {script}", flush=True)
        return
    cmd = [
        sys.executable,
        str(script),
        "--jump-host",
        args.jump_host or "",
        "--adb-bin",
        args.adb_bin,
        "--serial",
        args.serial,
        "--out-dir",
        str(out_dir),
        "--snapshot-name",
        snapshot_name,
    ]
    print("+ " + shlex.join(cmd), flush=True)
    subprocess.run(cmd, check=False)


def check_shared_arena_props(adb: Adb, apps: list[App], expected_wrap: str, require: bool) -> None:
    packages = " ".join(shlex.quote(app.package) for app in apps)
    cmd = (
        f"for p in {packages}; do "
        "printf 'wrap.%s=' \"$p\"; getprop \"wrap.$p\"; "
        "done"
    )
    out = adb.shell(cmd, check=False, timeout=30)
    print("== wrap props ==", flush=True)
    print(out.strip(), flush=True)
    if require:
        missing = []
        for line in out.splitlines():
            if not line.startswith("wrap.") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            if value.strip() != expected_wrap:
                missing.append(f"{key}={value.strip() or '<empty>'}")
        if missing:
            raise RuntimeError(
                "shared arena wrap props are not ready: "
                + ", ".join(missing)
                + f"; expected {expected_wrap}"
            )


def main() -> int:
    args = parse_args()
    os.environ.setdefault("ADB_SERVER_SOCKET", "tcp:127.0.0.1:15037")
    print(f"ADB_SERVER_SOCKET={os.environ['ADB_SERVER_SOCKET']}", flush=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = Path(args.out_dir) / f"app-switch-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)

    adb = Adb(args.adb_bin, args.serial, args.jump_host or None, args.dry_run)
    apps = [
        App(key, name, getattr(args, f"pkg_{key}"))
        for key, (name, _) in DEFAULT_APPS.items()
    ]
    by_key = {app.key: app for app in apps}
    results: list[LaunchResult] = []
    preload_checks: list[PreloadCheck] = []
    pre_boot_marker = ""

    try:
        adb.run(["get-state"], timeout=20)
        pre_boot_marker = collect_boot_marker(adb, out_dir, "pre")
        width, height = parse_wm_size(adb.shell("wm size", check=False, timeout=10))
        print(f"screen={width}x{height}", flush=True)

        print("== resolve launcher activities ==", flush=True)
        for app in apps:
            app.component = resolve_activity(adb, app)
            if app.component:
                print(f"{app.name}: {app.package} -> {app.component}", flush=True)
            else:
                print(f"WARNING: cannot resolve launcher activity: {app.name} {app.package}", flush=True)

        if args.check_shared_arena_props:
            check_shared_arena_props(adb, apps, args.expected_wrap, args.require_shared_arena_wrap)

        if args.dry_run and args.resolve_only:
            print(f"dry_run_resolve_only=1 out_dir={out_dir}", flush=True)
            return 0

        unresolved = [app for app in apps if not app.component]
        if unresolved:
            names = ", ".join(f"{app.name}({app.package})" for app in unresolved)
            raise RuntimeError(f"cannot resolve launcher activity: {names}")

        if args.resolve_only:
            print(f"resolve_only=1 out_dir={out_dir}", flush=True)
            return 0

        if args.force_stop_before_run:
            for app in apps:
                adb.shell(f"am force-stop {shlex.quote(app.package)}", check=False, timeout=15)

        if args.clear_logcat:
            adb.run(["logcat", "-c"], check=False, timeout=20)

        background_sequence = ["taobao", "qqlive", "alibaba", "baidu", "tomato", "amap", "weibo"]

        for round_idx in range(1, args.rounds + 1):
            prefix = f"r{round_idx}"
            print(f"== round {round_idx} ==", flush=True)

            label = f"{prefix}_bili_start"
            results.append(start_app(adb, by_key["bili"], label))
            preload_checks.extend(collect_preload_check(adb, by_key["bili"], label, out_dir))
            time.sleep(args.sleep_after_start)
            if args.stop_after_bili_start:
                break
            tap_frac(adb, width, height, args.bili_video_tap_x, args.bili_video_tap_y)
            time.sleep(4.0)
            if args.stop_after_bili_video_tap:
                break
            home(adb, args.sleep_after_home)

            label = f"{prefix}_qqmusic_start"
            results.append(start_app(adb, by_key["qqmusic"], label))
            preload_checks.extend(collect_preload_check(adb, by_key["qqmusic"], label, out_dir))
            time.sleep(args.sleep_after_start)
            if args.stop_after_qqmusic_start:
                break
            media_play(adb)
            tap_frac(adb, width, height, 0.50, 0.88)
            time.sleep(2.0)
            if args.stop_after_qqmusic_media:
                break
            home(adb, args.sleep_after_home)

            for bg_idx, key in enumerate(background_sequence):
                if args.max_background_apps >= 0 and bg_idx >= args.max_background_apps:
                    break
                label = f"{prefix}_{key}_background"
                results.append(start_app(adb, by_key[key], label))
                preload_checks.extend(collect_preload_check(adb, by_key[key], label, out_dir))
                time.sleep(args.sleep_after_start)
                home(adb, args.sleep_after_home)

            if args.collect_memory:
                collect_memory_snapshot(args, out_dir, f"memory-after-background-setup-{prefix}")

            label = f"{prefix}_xhs_foreground"
            results.append(start_app(adb, by_key["xhs"], label))
            preload_checks.extend(collect_preload_check(adb, by_key["xhs"], label, out_dir))
            time.sleep(args.sleep_after_start)
            if args.collect_memory:
                collect_memory_snapshot(args, out_dir, f"memory-before-switch-{prefix}")
            label = f"{prefix}_xhs_to_douyin"
            results.append(start_app(adb, by_key["douyin"], label))
            preload_checks.extend(collect_preload_check(adb, by_key["douyin"], label, out_dir))
            time.sleep(args.sleep_after_start)

        collect_boot_marker(adb, out_dir, "post")
        write_results(out_dir, results)
        write_preload_checks(out_dir, preload_checks)
        lmkd_count, lmkd_kill_count = collect_lmkd(adb, out_dir, args.logcat_lines)
    except Exception as exc:
        if results:
            write_results(out_dir, results)
        if preload_checks:
            write_preload_checks(out_dir, preload_checks)
        (out_dir / "failure.txt").write_text(f"{type(exc).__name__}: {exc}\n", encoding="utf-8")
        print(f"ERROR: {type(exc).__name__}: {exc}", flush=True)
        if args.collect_reboot_diagnostics:
            collect_reboot_diagnostics(adb, out_dir, args, "failure", pre_boot_marker)
        raise

    final = [r for r in results if r.label.endswith("xhs_to_douyin")]
    print("== summary ==", flush=True)
    for r in final:
        print(
            f"{r.label}: {r.app_name} wait_time_ms={r.wait_time_ms} "
            f"total_time_ms={r.total_time_ms} state={r.launch_state}",
            flush=True,
        )
    print(f"lmkd_log_hits={lmkd_count}", flush=True)
    print(f"lmkd_kill_hits={lmkd_kill_count}", flush=True)
    print(f"out_dir={out_dir}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
