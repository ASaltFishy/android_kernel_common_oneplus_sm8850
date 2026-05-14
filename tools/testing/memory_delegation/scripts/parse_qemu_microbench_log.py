#!/usr/bin/env python3

import argparse
import csv
import importlib.util
from pathlib import Path
from types import SimpleNamespace


DEFAULT_MICROBENCH_SCRIPT = Path(
    "/home/lrc/patent/llvm-20/compiler-rt/lib/scudo/standalone/run_shared_arena_microbench.py"
)
DEFAULT_START_MARKER = "Running scudo_shared_arena_latency_bench ..."


def load_microbench_module(script_path):
    spec = importlib.util.spec_from_file_location("microbench", script_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 microbench 脚本: {script_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_summary(records, pd):
    df = pd.DataFrame([record.__dict__ for record in records])
    summary_df = (
        df.groupby(
            ["threads", "size_human", "size_bytes", "path", "metric"], as_index=False
        )
        .agg(
            mean_ns=("mean_ns", "mean"),
            std_ns=("mean_ns", "std"),
            route_hint=("route_hint", "first"),
            secondary_cache_calls=("secondary_cache_calls", "first"),
            secondary_cache_hits=("secondary_cache_hits", "first"),
            secondary_cache_hit_rate=("secondary_cache_hit_rate", "first"),
        )
        .fillna({"std_ns": 0.0})
    )
    return df, summary_df


def parse_args():
    parser = argparse.ArgumentParser(
        description="从 QEMU 串口日志中提取 SharedArena microbench 结果并生成图表"
    )
    parser.add_argument("--log", required=True, help="QEMU 串口日志路径")
    parser.add_argument("--results-dir", required=True, help="输出结果目录")
    parser.add_argument(
        "--microbench-script",
        default=str(DEFAULT_MICROBENCH_SCRIPT),
        help="Scudo microbench 主脚本路径",
    )
    parser.add_argument(
        "--raw-name",
        default="qemu-serial-benchmark.txt",
        help="截取后的 benchmark 原始文本文件名",
    )
    parser.add_argument(
        "--start-marker",
        default=DEFAULT_START_MARKER,
        help="从日志中截取 benchmark 输出的起始标记；传空串表示保留整个日志",
    )
    parser.add_argument("--remote-host", default="QEMU aarch64")
    parser.add_argument("--device-dir", default="/initramfs/bin")
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--iterations", type=int, default=80)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--threads", type=int, required=True, help="本份日志对应的线程数")
    parser.add_argument("--sizes", required=True, help="逗号分隔的 bytes 尺寸列表")
    return parser.parse_args()


def main():
    args = parse_args()

    script_path = Path(args.microbench_script).resolve()
    log_path = Path(args.log).resolve()
    results_dir = Path(args.results_dir).resolve()
    raw_dir = results_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)

    module = load_microbench_module(script_path)

    text = log_path.read_text(encoding="utf-8", errors="replace").replace("\r", "")
    if args.start_marker:
        start = text.find(args.start_marker)
        bench_text = text[start:] if start >= 0 else text
    else:
        bench_text = text

    (raw_dir / args.raw_name).write_text(bench_text, encoding="utf-8")

    records = module.parse_benchmark_output(
        bench_text, repetition=args.repetitions, threads=args.threads
    )
    raw_df, summary_df = build_summary(records, module.pd)

    raw_df.to_csv(
        results_dir / "raw_results.csv",
        index=False,
        quoting=csv.QUOTE_MINIMAL,
    )
    summary_df.to_csv(
        results_dir / "summary.csv",
        index=False,
        quoting=csv.QUOTE_MINIMAL,
    )

    module.plot_results(summary_df, results_dir)

    summary_args = SimpleNamespace(
        remote_host=args.remote_host,
        device_dir=args.device_dir,
        repetitions=args.repetitions,
        iterations=args.iterations,
        warmup=args.warmup,
        thread_counts=str(args.threads),
        sizes=args.sizes,
    )
    module.write_summary_md(summary_df, results_dir / "summary.md", summary_args)


if __name__ == "__main__":
    main()
