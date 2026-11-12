#!/usr/bin/env python3
"""Summarize KBench fixed-size trials using native Homa metrics deltas."""
import csv
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("Usage: summarize_sweep.py OUTPUT_DIR")
root = Path(sys.argv[1])
with (root / "manifest.csv").open(newline="") as f:
    entries = list(csv.DictReader(f))
if not entries:
    raise SystemExit("No complete experiments in manifest.csv")

RX_STAGES = ("add_packet", "copy_to_pool", "actor_copy",
             "app_copy", "skb_free")
TX_STAGES = {
    # TX temp[0..6] are the pre-existing lightweight counters in
    # homa_outgoing.c (not Timetrace ring-buffer entries).
    "tx_outfill": ("temp0", "temp4", "sent_msg_bytes"),
    "tx_packet_alloc": ("temp1", None, None),
    "tx_homa_xmit_data": ("temp2", None, None),
    "tx_stash_pages": ("temp3", None, None),
    "tx_xmit_data": ("temp5", "temp6", None),
}
KEYS = {
    f"rx_{stage}_{field}"
    for stage in RX_STAGES for field in ("ns", "count", "bytes")
}
KEYS.update({"temp0", "temp1", "temp2", "temp3",
             "temp4", "temp5", "temp6", "sent_msg_bytes"})

def result_value(text, pattern, caster=float):
    match = re.search(pattern, text, flags=re.MULTILINE)
    return caster(match.group(1)) if match else None

def metrics_snapshot(path):
    """Sum counters across all CPU sections of /proc/net/homa_metrics."""
    counters = defaultdict(int)
    for line in path.read_text().splitlines():
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s+(\d+)\b", line)
        if match and match.group(1) in KEYS:
            counters[match.group(1)] += int(match.group(2))
    return counters

def metrics_delta(before_path, after_path):
    before = metrics_snapshot(before_path)
    after = metrics_snapshot(after_path)
    missing = KEYS - set(after)
    if missing:
        raise ValueError(f"Metrics missing in {after_path}: {sorted(missing)}")
    delta = {}
    for name in KEYS:
        value = after[name] - before[name]
        if value < 0:
            raise ValueError(
                f"Counter decreased in {after_path}: {name} "
                f"{before[name]} -> {after[name]}. Was Homa reloaded?")
        delta[name] = value
    return delta

def per_stage(counters, stage, name_ns, name_count, name_bytes):
    ns_total = counters[name_ns]
    count = counters[name_count] if name_count else None
    nbytes = counters[name_bytes] if name_bytes else None
    return {
        "stage": stage,
        "count": count,
        "ns_total": ns_total,
        "avg_ns": round(ns_total / count, 3) if count else None,
        "bytes": nbytes,
        "ns_per_4k": round(ns_total * 4096 / nbytes, 3)
                     if nbytes else None,
    }

def write_csv(path, rows, columns):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

runs, stages, stage_index = [], [], {}
for entry in entries:
    size = int(entry["size_bytes"])
    rx = int(entry["rx_single_copy"])
    repeat = int(entry["repeat"])
    text = (root / entry["client_result"]).read_text()
    p99 = result_value(text, r"^\s*P99:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    p50 = result_value(text, r"^\s*P50:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    achieved = result_value(text, r"throughput:\s*(\d+)\s+RPCs/sec", int)
    failures = result_value(text, r"failed_rpc=(\d+)", int)
    skipped = result_value(text, r"skipped_busy_total=(\d+)", int)
    completed = result_value(text, r"completed RPCs:\s*(\d+)", int)
    valid = p99 is not None and achieved is not None and failures == 0
    run = {
        "size_bytes": size, "size_kib": size // 1024,
        "rx_single_copy": rx, "repeat": repeat, "valid": int(valid),
        "p50_us": p50, "p99_us": p99,
        "achieved_rps": achieved, "completed": completed,
        "failed_rpc": failures, "skipped_busy_total": skipped,
    }
    runs.append(run)
    for side in ("client", "server"):
        delta = metrics_delta(
            root / entry[f"{side}_before"],
            root / entry[f"{side}_after"],
        )
        rows = [
            per_stage(delta, name,
                      f"rx_{name}_ns", f"rx_{name}_count",
                      f"rx_{name}_bytes")
            for name in RX_STAGES
        ]
        rows.extend(
            per_stage(delta, name, *sources)
            for name, sources in TX_STAGES.items()
        )
        for row in rows:
            item = dict(size_bytes=size, size_kib=size // 1024,
                        rx_single_copy=rx, repeat=repeat, side=side, **row)
            stages.append(item)
            stage_index[(size, rx, repeat, side, row["stage"])] = row

write_csv(root / "runs.csv", runs, [
    "size_bytes", "size_kib", "rx_single_copy", "repeat", "valid",
    "p50_us", "p99_us", "achieved_rps", "completed", "failed_rpc",
    "skipped_busy_total",
])
write_csv(root / "stages.csv", stages, [
    "size_bytes", "size_kib", "rx_single_copy", "repeat", "side",
    "stage", "count", "ns_total", "avg_ns", "bytes", "ns_per_4k",
])

groups = defaultdict(list)
for run in runs:
    groups[(run["size_bytes"], run["rx_single_copy"])].append(run)

summary, stage_columns = [], [
    f"{side}_{stage}_ns4k"
    for side in ("client", "server") for stage in RX_STAGES
]
stage_columns.extend(
    f"{side}_{stage}_avg_ns"
    for side in ("client", "server") for stage in TX_STAGES
)
for (size, rx), group in sorted(groups.items()):
    valid = [r for r in group if r["valid"]]
    p99s = [r["p99_us"] for r in valid]
    rates = [r["achieved_rps"] for r in valid]
    output = {
        "size_bytes": size, "size_kib": size // 1024,
        "rx_single_copy": rx, "repeats": len(group),
        "valid_repeats": len(valid),
        "median_p99_us": statistics.median(p99s) if p99s else None,
        "mean_p99_us": round(statistics.mean(p99s), 3) if p99s else None,
        "stdev_p99_us": round(statistics.stdev(p99s), 3)
                        if len(p99s) > 1 else None,
        "mean_achieved_rps": round(statistics.mean(rates), 1) if rates else None,
    }
    for side in ("client", "server"):
        for stage in RX_STAGES:
            values = [
                stage_index[(size, rx, run["repeat"], side, stage)]["ns_per_4k"]
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{stage}_ns4k"] = (
                round(statistics.mean(values), 3) if values else None
            )
        for stage in TX_STAGES:
            values = [
                stage_index[(size, rx, run["repeat"], side, stage)]["avg_ns"]
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{stage}_avg_ns"] = (
                round(statistics.mean(values), 3) if values else None
            )
    summary.append(output)

write_csv(root / "summary.csv", summary, [
    "size_bytes", "size_kib", "rx_single_copy", "repeats", "valid_repeats",
    "median_p99_us", "mean_p99_us", "stdev_p99_us", "mean_achieved_rps",
    *stage_columns,
])

for row in summary:
    print(f'{row["size_kib"]:>5} KiB RX={row["rx_single_copy"]} '
          f'valid={row["valid_repeats"]}/{row["repeats"]} '
          f'median P99={row["median_p99_us"]} us '
          f'mean achieved={row["mean_achieved_rps"]} RPC/s')
print(f"Wrote {root / 'runs.csv'}, {root / 'stages.csv'}, "
      f"{root / 'summary.csv'}")
