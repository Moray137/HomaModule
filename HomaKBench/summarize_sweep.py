#!/usr/bin/env python3
"""Summarize KBench fixed-size and blksw6 trials with Homa metrics deltas."""
import csv
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit("Usage: summarize_sweep.py OUTPUT_DIR")
root = Path(sys.argv[1])
MIX_SIZES_KIB = (4, 8, 12, 16, 128, 1024)
MAX_LATENCY_SAMPLES = 2 * 1024 * 1024  # kbench_client.c MAX_SAMPLES
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

# Optional on older snapshots. New runs include these metrics automatically.
RX_EXTRA_KEYS = (
    "rx_actor_order_walk_steps",
    "rx_actor_order_packets",
    "rx_actor_delivery_setup_ns",
    "rx_actor_delivery_overhead_ns",
    "rx_actor_deliveries",
    "rx_actor_handoffs",
    "rx_pool_handoffs",
    "rx_pool_partial_handoffs",
    "rx_pool_copy_batches",
    "rx_pool_batch_overhead_ns",
)
RX_EXTRA_STAGES = {
    "actor_delivery_setup": ("rx_actor_delivery_setup_ns",
                             "rx_actor_deliveries"),
    "actor_delivery_overhead": ("rx_actor_delivery_overhead_ns",
                                "rx_actor_deliveries"),
    "pool_batch_overhead": ("rx_pool_batch_overhead_ns",
                            "rx_pool_copy_batches"),
}
# Include every RX counter and useful per-RPC/packet/batch normalization
# directly in summary.csv. metrics_all.csv retains every raw run delta.
RX_EXTRA_SUMMARY_FIELDS = (
    "rx_messages", *RX_EXTRA_KEYS,
    "walk_steps_per_packet",
    "pool_batches_per_rpc",
    "partial_handoffs_per_rpc",
    "partial_pct_of_counted_handoffs",
    "actor_setup_ns_per_delivery",
    "actor_overhead_ns_per_delivery",
    "actor_overhead_ns_per_rpc",
    "pool_overhead_ns_per_batch",
    "pool_overhead_ns_per_rpc",
)

def result_value(text, pattern, caster=float):
    match = re.search(pattern, text, flags=re.MULTILINE)
    return caster(match.group(1)) if match else None

def parse_size_results(text):
    """Read the final debugfs per-size latency lines (mixed workload only)."""
    pattern = (r"^\s*(\d+) KiB:\s*n=(\d+)\s+"
               r"p50=([0-9]+(?:\.[0-9]+)?)\s+us\s+"
               r"p99=([0-9]+(?:\.[0-9]+)?)\s+us\s*$")
    rows = {}
    for match in re.finditer(pattern, text, flags=re.MULTILINE):
        size, count, p50, p99 = match.groups()
        size = int(size)
        if size not in MIX_SIZES_KIB or size in rows:
            raise ValueError(f"Unexpected or duplicate blksw6 size: {size} KiB")
        rows[size] = {"samples": int(count),
                      "p50_us": float(p50), "p99_us": float(p99)}
    return rows

def metrics_snapshot(path):
    """Sum every per-CPU Homa metric; ignore global clock metadata and core IDs."""
    counters = defaultdict(int)
    in_cpu_section = False
    for line in path.read_text().splitlines():
        match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s+(\d+)\b", line)
        if not match:
            continue
        name, value = match.group(1), int(match.group(2))
        if name == "core":
            in_cpu_section = True
            continue
        if in_cpu_section:
            counters[name] += value
    return dict(counters)

def metrics_delta(before_path, after_path):
    before = metrics_snapshot(before_path)
    after = metrics_snapshot(after_path)
    if set(before) != set(after):
        raise ValueError(
            f"Metrics differ between {before_path} and {after_path}: "
            f"only_before={sorted(set(before) - set(after))}, "
            f"only_after={sorted(set(after) - set(before))}")
    missing = KEYS - set(after)
    if missing:
        raise ValueError(f"Metrics missing in {after_path}: {sorted(missing)}")
    delta = {name: after[name] - before[name] for name in after}
    # Existing stage counters are monotonically increasing. Other exported
    # fields may be snapshots/derived quantities, so preserve signed deltas.
    for name in KEYS:
        if delta[name] < 0:
            raise ValueError(
                f"Counter decreased in {after_path}: {name} "
                f"{before[name]} -> {after[name]}. Was Homa reloaded?")
    return before, after, delta

def ratio(numerator, denominator):
    return round(numerator / denominator, 3) if (
        numerator is not None and denominator) else None

def extra_breakdown(meta, delta):
    """RX-focused counters plus normalized per-message and per-batch costs.

    Message denominator includes warmup (same interval as Homa counters).
    The server's initial vanilla handoff occurs in homa_rpc_alloc_server
    and is not included in rx_pool_handoffs.
    """
    side = meta["side"]
    messages = delta.get("client_responses_done" if side == "client"
                         else "server_requests_done")
    packets = delta.get("rx_actor_order_packets")
    deliveries = delta.get("rx_actor_deliveries")
    batches = delta.get("rx_pool_copy_batches")
    handoffs = delta.get("rx_pool_handoffs")
    partial = delta.get("rx_pool_partial_handoffs")
    setup = delta.get("rx_actor_delivery_setup_ns")
    actor_overhead = delta.get("rx_actor_delivery_overhead_ns")
    pool_overhead = delta.get("rx_pool_batch_overhead_ns")
    return {
        **meta, "rx_messages": messages,
        **{key: delta.get(key) for key in RX_EXTRA_KEYS},
        "walk_steps_per_packet": ratio(
            delta.get("rx_actor_order_walk_steps"), packets),
        "pool_batches_per_rpc": ratio(batches, messages),
        "partial_handoffs_per_rpc": ratio(partial, messages),
        "partial_pct_of_counted_handoffs": ratio(
            partial * 100 if partial is not None else None, handoffs),
        "actor_setup_ns_per_delivery": ratio(setup, deliveries),
        "actor_overhead_ns_per_delivery": ratio(actor_overhead, deliveries),
        "actor_overhead_ns_per_rpc": ratio(actor_overhead, messages),
        "pool_overhead_ns_per_batch": ratio(pool_overhead, batches),
        "pool_overhead_ns_per_rpc": ratio(pool_overhead, messages),
    }

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

runs, stages, stage_index, size_runs = [], [], {}, []
all_metrics_rows, extra_index = [], {}
for entry in entries:
    workload = entry.get("workload") or "fixed"  # Old fixed-size manifests.
    if workload not in ("fixed", "blksw6"):
        raise ValueError(f"Unknown workload: {workload}")
    size = int(entry["size_bytes"]) if entry["size_bytes"] else None
    if (workload == "fixed") != (size is not None):
        raise ValueError(f"Invalid size/workload in manifest: {entry}")
    rx = int(entry["rx_single_copy"])
    repeat = int(entry["repeat"])
    text = (root / entry["client_result"]).read_text()
    reported_workload = result_value(text, r"\bworkload=(\w+)", str)
    reported_size = result_value(text, r"\bfixed_msg_size=(\d+)", int)
    if reported_workload != workload and not (
            reported_workload is None and workload == "fixed"):
        raise ValueError(f"Workload mismatch in {entry['client_result']}: "
                         f"manifest={workload}, client={reported_workload}")
    if workload == "fixed" and reported_size is not None and reported_size != size:
        raise ValueError(f"Size mismatch in {entry['client_result']}: "
                         f"manifest={size}, client={reported_size}")
    p99 = result_value(text, r"^\s*P99:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    p50 = result_value(text, r"^\s*P50:\s*([0-9]+(?:\.[0-9]+)?)\s+us")
    achieved = result_value(text, r"throughput:\s*(\d+)\s+RPCs/sec", int)
    per_worker = result_value(text, r"rate_per_worker=(\d+)", int)
    aggregate_target = result_value(text, r"total_offered_rate=(\d+)", int)
    # New manifest stores rate explicitly. Check it against the client
    # output to prevent mixing/mislabeling rate sweep results.
    if entry.get("rate_per_worker"):
        recorded_rate = int(entry["rate_per_worker"])
        recorded_target = int(entry["target_aggregate_rps"])
        if per_worker != recorded_rate or aggregate_target != recorded_target:
            raise ValueError(
                f"Rate mismatch in {entry['client_result']}: "
                f"manifest={recorded_rate}/{recorded_target}, "
                f"client={per_worker}/{aggregate_target}")
        per_worker = recorded_rate
        aggregate_target = recorded_target
    offered = result_value(text, r"offered_slots_total=(\d+)", int)
    failures = result_value(text, r"failed_rpc=(\d+)", int)
    skipped = result_value(text, r"skipped_busy_total=(\d+)", int)
    completed = result_value(text, r"completed RPCs:\s*(\d+)", int)
    duration_match = re.search(r"\bduration=(\d+)s warmup=(\d+)s", text)
    measurement_secs = ((int(duration_match.group(1)) -
                         int(duration_match.group(2)))
                        if duration_match else None)
    class_results = parse_size_results(text) if workload == "blksw6" else {}
    size_complete = (sum(row["samples"] for row in class_results.values())
                     == completed) if workload == "blksw6" else True
    sample_cap_hit = completed is not None and completed >= MAX_LATENCY_SAMPLES
    valid = (p50 is not None and p99 is not None and achieved is not None
             and failures == 0 and not sample_cap_hit and size_complete
             and (workload != "blksw6" or
                  (measurement_secs is not None and measurement_secs > 0)))
    run = {
        "workload": workload, "size_bytes": size if size is not None else "",
        "size_kib": size // 1024 if size is not None else "",
        "rx_single_copy": rx, "repeat": repeat, "valid": int(valid),
        "sample_cap_hit": int(sample_cap_hit),
        "size_samples_complete": int(size_complete),
        "p50_us": p50, "p99_us": p99,
        "rate_per_worker": per_worker, "target_aggregate_rps": aggregate_target,
        "offered_slots_total": offered,
        "achieved_rps": achieved, "completed": completed,
        "failed_rpc": failures, "skipped_busy_total": skipped,
    }
    runs.append(run)
    if workload == "blksw6":
        for class_kib in MIX_SIZES_KIB:
            data = class_results.get(class_kib)
            n_class = data["samples"] if data else 0
            size_runs.append({
                "workload": workload, "size_bytes": class_kib * 1024,
                "size_kib": class_kib,
                "rx_single_copy": rx, "repeat": repeat,
                "rate_per_worker": per_worker,
                "target_aggregate_rps": aggregate_target,
                "valid": int(valid), "samples": n_class,
                "sample_fraction": round(n_class / completed, 6)
                                   if completed else None,
                "achieved_class_rps": round(n_class / measurement_secs, 3)
                                      if measurement_secs and measurement_secs > 0 else None,
                "p50_us": data["p50_us"] if data else None,
                "p99_us": data["p99_us"] if data else None,
            })
    for side in ("client", "server"):
        before, after, delta = metrics_delta(
            root / entry[f"{side}_before"],
            root / entry[f"{side}_after"],
        )
        meta = dict(workload=workload,
                    size_bytes=size if size is not None else "",
                    size_kib=size // 1024 if size is not None else "",
                    rx_single_copy=rx, rate_per_worker=per_worker,
                    target_aggregate_rps=aggregate_target,
                    repeat=repeat, valid=int(valid), side=side)
        all_metrics_rows.extend({
            **meta, "metric": name, "before": before[name],
            "after": after[name], "delta": delta[name],
        } for name in sorted(delta))
        extra_index[(workload, size, rx, per_worker, repeat, side)] = (
            extra_breakdown(meta, delta))
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
        rows.extend(
            per_stage(delta, name, ns_key, count_key, None)
            for name, (ns_key, count_key) in RX_EXTRA_STAGES.items()
            if ns_key in delta and count_key in delta
        )
        for row in rows:
            item = dict(workload=workload,
                        size_bytes=size if size is not None else "",
                        size_kib=size // 1024 if size is not None else "",
                        rx_single_copy=rx, rate_per_worker=per_worker,
                        target_aggregate_rps=aggregate_target,
                        repeat=repeat, side=side, **row)
            stages.append(item)
            stage_index[(workload, size, rx, per_worker, repeat, side, row["stage"])] = row

write_csv(root / "runs.csv", runs, [
    "workload", "size_bytes", "size_kib", "rx_single_copy", "repeat", "valid",
    "sample_cap_hit", "size_samples_complete",
    "p50_us", "p99_us", "rate_per_worker", "target_aggregate_rps",
    "offered_slots_total", "achieved_rps", "completed", "failed_rpc",
    "skipped_busy_total",
])
write_csv(root / "stages.csv", stages, [
    "workload", "size_bytes", "size_kib", "rx_single_copy",
    "rate_per_worker", "target_aggregate_rps", "repeat", "side",
    "stage", "count", "ns_total", "avg_ns", "bytes", "ns_per_4k",
])

groups = defaultdict(list)
for run in runs:
    size_key = run["size_bytes"] if run["workload"] == "fixed" else None
    groups[(run["workload"], size_key,
            run["rx_single_copy"], run["rate_per_worker"])].append(run)

summary, stage_columns = [], [
    f"{side}_{stage}_ns4k"
    for side in ("client", "server") for stage in RX_STAGES
]
stage_columns.extend(
    f"{side}_{stage}_avg_ns"
    for side in ("client", "server") for stage in TX_STAGES
)
stage_columns.extend(
    f"{side}_{stage}_avg_ns"
    for side in ("client", "server") for stage in RX_EXTRA_STAGES
)
stage_columns.extend(
    f"{side}_{field}"
    for side in ("client", "server") for field in RX_EXTRA_SUMMARY_FIELDS
)
for (workload, size, rx, rate), group in sorted(
        groups.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0,
                                       kv[0][2], kv[0][3] or -1)):
    valid = [r for r in group if r["valid"]]
    p50s = [r["p50_us"] for r in valid if r["p50_us"] is not None]
    p99s = [r["p99_us"] for r in valid]
    rates = [r["achieved_rps"] for r in valid]
    skips = [r["skipped_busy_total"] for r in valid
             if r["skipped_busy_total"] is not None]
    output = {
        "workload": workload,
        "size_bytes": size if size is not None else "",
        "size_kib": size // 1024 if size is not None else "",
        "rx_single_copy": rx, "repeats": len(group),
        "valid_repeats": len(valid),
        "rate_per_worker": rate,
        "target_aggregate_rps": group[0]["target_aggregate_rps"],
        "median_p50_us": statistics.median(p50s) if p50s else None,
        "median_p99_us": statistics.median(p99s) if p99s else None,
        "mean_p99_us": round(statistics.mean(p99s), 3) if p99s else None,
        "stdev_p99_us": round(statistics.stdev(p99s), 3)
                        if len(p99s) > 1 else None,
        "mean_achieved_rps": round(statistics.mean(rates), 1) if rates else None,
        "mean_skipped_busy_total": round(statistics.mean(skips), 1)
                                   if skips else None,
    }
    for side in ("client", "server"):
        for stage in RX_STAGES:
            values = [
                stage_index[(workload, size, rx, rate, run["repeat"], side, stage)]["ns_per_4k"]
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{stage}_ns4k"] = (
                round(statistics.mean(values), 3) if values else None
            )
        for stage in TX_STAGES:
            values = [
                stage_index[(workload, size, rx, rate, run["repeat"], side, stage)]["avg_ns"]
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{stage}_avg_ns"] = (
                round(statistics.mean(values), 3) if values else None
            )
        for stage in RX_EXTRA_STAGES:
            values = [
                stage_index.get(
                    (workload, size, rx, rate, run["repeat"], side, stage),
                    {}).get("avg_ns")
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{stage}_avg_ns"] = (
                round(statistics.mean(values), 3) if values else None
            )
        for field in RX_EXTRA_SUMMARY_FIELDS:
            values = [
                extra_index[(workload, size, rx, rate, run["repeat"], side)][field]
                for run in valid
            ]
            values = [v for v in values if v is not None]
            output[f"{side}_{field}"] = (
                round(statistics.mean(values), 3) if values else None
            )
    summary.append(output)

write_csv(root / "summary.csv", summary, [
    "workload", "size_bytes", "size_kib",
    "rx_single_copy", "repeats", "valid_repeats",
    "rate_per_worker", "target_aggregate_rps",
    "median_p50_us", "median_p99_us", "mean_p99_us", "stdev_p99_us",
    "mean_achieved_rps", "mean_skipped_busy_total",
    *stage_columns,
])

# Per-size latency rows are separate: RX/TX Homa stage counters
# are per-node aggregate and cannot be attributed to individual sizes.
write_csv(root / "size_runs.csv", size_runs, [
    "workload", "size_bytes", "size_kib", "rx_single_copy", "repeat",
    "rate_per_worker", "target_aggregate_rps", "valid",
    "samples", "sample_fraction", "achieved_class_rps", "p50_us", "p99_us",
])

class_groups = defaultdict(list)
for row in size_runs:
    class_groups[(row["rx_single_copy"], row["rate_per_worker"],
                  row["size_bytes"])].append(row)

size_summary = []
for (rx, rate, size), group in sorted(class_groups.items()):
    valid_rows = [row for row in group if row["valid"]]
    nonempty = [row for row in valid_rows if row["samples"] > 0]
    p50s = [row["p50_us"] for row in nonempty]
    p99s = [row["p99_us"] for row in nonempty]
    size_summary.append({
        "workload": "blksw6", "size_bytes": size,
        "size_kib": size // 1024, "rx_single_copy": rx,
        "rate_per_worker": rate,
        "target_aggregate_rps": group[0]["target_aggregate_rps"],
        "repeats": len(group), "valid_repeats": len(valid_rows),
        "nonempty_repeats": len(nonempty),
        "total_samples": sum(row["samples"] for row in valid_rows),
        "mean_samples": round(statistics.mean(
            row["samples"] for row in valid_rows), 1) if valid_rows else None,
        "mean_sample_fraction": round(statistics.mean(
            row["sample_fraction"] for row in valid_rows), 6) if valid_rows else None,
        "mean_achieved_class_rps": round(statistics.mean(
            row["achieved_class_rps"] for row in valid_rows), 3)
            if valid_rows else None,
        "median_p50_us": statistics.median(p50s) if p50s else None,
        "median_p99_us": statistics.median(p99s) if p99s else None,
        "mean_p99_us": round(statistics.mean(p99s), 3) if p99s else None,
        "stdev_p99_us": round(statistics.stdev(p99s), 3)
                        if len(p99s) > 1 else None,
    })
write_csv(root / "size_summary.csv", size_summary, [
    "workload", "size_bytes", "size_kib", "rx_single_copy",
    "rate_per_worker", "target_aggregate_rps", "repeats", "valid_repeats",
    "nonempty_repeats", "total_samples", "mean_samples",
    "mean_sample_fraction", "mean_achieved_class_rps",
    "median_p50_us", "median_p99_us", "mean_p99_us", "stdev_p99_us",
])


# Export every per-CPU /proc/net/homa_metrics delta, not just stages.
# These are global node-wide counters over each run (including warmup).
metric_columns = [
    "workload", "size_bytes", "size_kib", "rx_single_copy",
    "rate_per_worker", "target_aggregate_rps", "repeat", "valid",
    "side", "metric", "before", "after", "delta",
]
write_csv(root / "metrics_all.csv", all_metrics_rows, metric_columns)

for row in summary:
    label = (f'{row["size_kib"]:>5} KiB' if row["workload"] == "fixed"
             else "blksw6")
    print(f'{label} RX={row["rx_single_copy"]} '
          f'rate/worker={row["rate_per_worker"]} '
          f'target={row["target_aggregate_rps"]} RPC/s '
          f'valid={row["valid_repeats"]}/{row["repeats"]} '
          f'median P50={row["median_p50_us"]} us '
          f'median P99={row["median_p99_us"]} us '
          f'mean achieved={row["mean_achieved_rps"]} RPC/s')
print(f"Wrote {root / 'runs.csv'}, {root / 'stages.csv'}, "
      f"{root / 'summary.csv'}, {root / 'size_runs.csv'}, "
      f"{root / 'size_summary.csv'}, {root / 'metrics_all.csv'}")
