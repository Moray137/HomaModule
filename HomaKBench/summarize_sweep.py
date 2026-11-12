#!/usr/bin/env python3
"""Parse KBench runs and Homa transport stage snapshots (stdlib only)."""
import csv
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("Usage: summarize_sweep.py OUTPUT_DIR")
root = Path(sys.argv[1])
entries = list(csv.DictReader((root / "manifest.csv").open()))
if not entries:
    raise SystemExit("No complete experiments in manifest.csv")

def match(text, regex, cast=float):
    found = re.search(regex, text, flags=re.MULTILINE)
    return cast(found.group(1)) if found else None

def parse_stages(path):
    output = []
    text = path.read_text()
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) != 8 or parts[0] in {"stage", "enable"}:
            continue
        try:
            output.append({
                "stage": parts[0],
                "count": int(parts[1]),
                "avg_ns": int(parts[2]),
                "stddev_ns": int(parts[3]),
                "max_ns": int(parts[4]),
                "bytes": int(parts[5]),
                "ns_per_4k": int(parts[6]),
                "avg_chunks": int(parts[7]),
            })
        except ValueError:
            continue
    return output

runs, stages = [], []
stage_index = {}
for row in entries:
    text = (root / row["client_result"]).read_text()
    size = int(row["size_bytes"])
    rx = int(row["rx_single_copy"])
    rep = int(row["repeat"])
    p99 = match(text, r"^\s*P99:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    p50 = match(text, r"^\s*P50:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    achieved = match(text, r"throughput:\s*([0-9]+)\s+RPCs/sec", int)
    failures = match(text, r"failed_rpc=([0-9]+)", int)
    skipped = match(text, r"skipped_busy_total=([0-9]+)", int)
    completed = match(text, r"completed RPCs:\s*([0-9]+)", int)
    valid = p99 is not None and achieved is not None and failures == 0
    run = {
        "size_bytes": size, "size_kib": size // 1024,
        "rx_single_copy": rx, "repeat": rep,
        "valid": int(valid),
        "p50_us": p50, "p99_us": p99,
        "achieved_rps": achieved, "completed": completed,
        "failed_rpc": failures, "skipped_busy_total": skipped,
    }
    runs.append(run)
    for side in ("client", "server"):
        for st in parse_stages(root / row[f"{side}_stats"]):
            item = dict(size_bytes=size, size_kib=size // 1024,
                        rx_single_copy=rx, repeat=rep, side=side, **st)
            stages.append(item)
            stage_index[(size, rx, rep, side, st["stage"])] = st

def write_csv(path, records, fields):
    with path.open("w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)

run_fields = list(runs[0])
write_csv(root / "runs.csv", runs, run_fields)
stage_fields = list(stages[0]) if stages else [
    "size_bytes", "size_kib", "rx_single_copy", "repeat", "side",
    "stage", "count", "avg_ns", "stddev_ns", "max_ns", "bytes",
    "ns_per_4k", "avg_chunks",
]
write_csv(root / "stages.csv", stages, stage_fields)

groups = defaultdict(list)
for run in runs:
    groups[(run["size_bytes"], run["rx_single_copy"])].append(run)

# Means of per-trial ns/4KB, not pooled latency quantiles.
wanted_stages = ("add_packet", "copy_to_pool", "actor_copy",
                 "app_copy", "skb_free")
stage_columns = [
    f"{side}_{stage}_ns4k"
    for side in ("client", "server")
    for stage in wanted_stages
]
summary = []
for (size, rx), group in sorted(groups.items()):
    good = [g for g in group if g["valid"]]
    p99s = [g["p99_us"] for g in good]
    rates = [g["achieved_rps"] for g in good]
    output = {
        "size_bytes": size, "size_kib": size // 1024,
        "rx_single_copy": rx, "repeats": len(group), "valid_repeats": len(good),
        "median_p99_us": statistics.median(p99s) if p99s else None,
        "mean_p99_us": round(statistics.mean(p99s), 3) if p99s else None,
        "stdev_p99_us": round(statistics.stdev(p99s), 3)
                        if len(p99s) > 1 else None,
        "mean_achieved_rps": round(statistics.mean(rates), 1) if rates else None,
    }
    for side in ("client", "server"):
        for stage in wanted_stages:
            vals = [
                stage_index[(size, rx, r["repeat"], side, stage)]["ns_per_4k"]
                for r in good if (size, rx, r["repeat"], side, stage) in stage_index
            ]
            output[f"{side}_{stage}_ns4k"] = (
                round(statistics.mean(vals), 2) if vals else None
            )
    summary.append(output)
summary_fields = [
    "size_bytes", "size_kib", "rx_single_copy", "repeats", "valid_repeats",
    "median_p99_us", "mean_p99_us", "stdev_p99_us", "mean_achieved_rps",
] + stage_columns
write_csv(root / "summary.csv", summary, summary_fields)

for s in summary:
    print(f'{s["size_kib"]:>5} KiB  RX={s["rx_single_copy"]} '
          f'valid={s["valid_repeats"]}/{s["repeats"]} '
          f'median P99={s["median_p99_us"]} us '
          f'mean achieved={s["mean_achieved_rps"]} rps')
print(f"Wrote {root / 'runs.csv'}, {root / 'stages.csv'}, "
      f"{root / 'summary.csv'}")
