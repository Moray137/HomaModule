# HomaKBench RX single-copy experiments

Build against the HomaModule branch and its matching Linux kernel headers:

```sh
cd HomaKBench
make KDIR=/lib/modules/$(uname -r)/build
```

## Workloads

The client supports `workload=fixed` (default) with `msg_size=<bytes>` and
`workload=blksw6` with these six request sizes:

| Size | Default probability (request count) |
|---|---:|
| 4 KiB | 10% |
| 8 KiB | 57% |
| 12 KiB | 10% |
| 16 KiB | 10% |
| 128 KiB | 8% |
| 1 MiB | 5% |

This is an **approximation**, not the original blksw trace's exact per-size
distribution: 4 KiB approximates the critical bottom 10% and 8 KiB is a
dominant class. Overrides are supported through
`blksw_weights=10,57,10,10,8,5` (must be six nonnegative integers summing
to 100). Each RPC chooses one size independently. The server replies with
the **same length** as the request. This models message-size variability,
not the original read/write ratio, queue mapping, or actual SSD behavior.

The client runs one CPU-bound kernel worker per socket, with at most one
outstanding RPC per worker. `rate` means **offered RPC/s per worker**
(similar to fio's per-job `rate_iops`), so aggregate target =
`rate * num_sockets`. Each worker has its own periodic pacing clock;
there is **no centralized round-robin pacer**. Initial phases are staggered
across workers to avoid a synchronous startup burst. When an RPC takes longer
than the worker's pacing interval, missed slots increment
`skipped_busy_total` instead of building a queue or sending catch-up bursts.
Total offered/skipped counts include warmup; aggregate achieved throughput
and latency samples exclude warmup. A worker is QD=1, not fio iodepth=16.

For the six sizes individually, use `workload=fixed` and the six values
`4096`, `8192`, `12288`, `16384`, `131072`, `1048576`.

## Independent TX and RX configuration

Use `tx_zc=0|1`, `rx_single_copy=0|1`, and
`nonblocking=0|1` on **both** client and server.
`tx_zc=1` uses MSG_SPLICE_PAGES and `rx_single_copy=1` registers
Homa's full-message skb actor. The vanilla path continues using the Homa
receive pool. The older client `zc` and server `rx_actor_used` parameters
remain supported as compatibility aliases.

**RX destination models:** the client uses page-backed `bio_vec[]`
(`rx_bvec=1`, representing the Host receive destination), while the
server uses a separately allocated page-backed Linux `scatterlist`
(representing the Target NVMe request SGL, without a storage backend or
DMA mapping). Both Server RX paths copy into the **same SGL layout**:
`rx_single_copy=1` copies directly `skb -> SGL` and
`rx_single_copy=0 simulate_app_copy=1` copies
`skb -> Homa pool -> SGL`. The server accepts `rx_bvec` for runner
compatibility but does not use it to select the destination. The
single-copy mode eliminates the intermediate pool-to-SGL copy;
it does not eliminate copying from the skb.

`nonblocking=1` uses `MSG_DONTWAIT` for each Homa TX/RX syscall,
retrying `-EAGAIN` only after waiting for socket readiness. This is
nonblocking **socket I/O**, with an event-waited worker per socket; it is
not yet an event-driven multi-RPC pipeline inside a single socket.
TCP's existing blocking benchmark path is unchanged.

Example (replace both IP addresses with the testbed's actual addresses):

```sh
# Server
sudo insmod kbench_server.ko transport=homa bind_ip=192.168.11.93 \
  tx_zc=0 rx_single_copy=1 nonblocking=1

# Client
sudo insmod kbench_client.ko transport=homa dst_ip=192.168.11.93 \
  workload=blksw6 blksw_weights=10,57,10,10,8,5 \
  num_sockets=16 rate=12 duration_sec=30 warmup_sec=5 \
  tx_zc=0 rx_single_copy=1 nonblocking=1
cat /sys/kernel/debug/homakbench/results
```

For an end-to-end pool-copy baseline that includes the former second copy,
run `rx_single_copy=0 simulate_app_copy=1` on **both** the client and
the server. Server-side `simulate_app_copy=1` copies pool bytes into
the worker-local SGL destination (the same layout used by the actor).
Do **not** enable `simulate_app_copy` for `rx_single_copy=1`.
Run the two-by-two TX/RX combinations without adding sorting as a separate
benchmark knob.

The client reports overall P50/P90/P99 and, for `blksw6`, per-size
sample count, P50, and P99. The experiment currently stores at most
`MAX_SAMPLES` latency samples; do not interpret a truncated run's
throughput as the full achieved throughput.

## Testing status

The changes have been reviewed against the source but have **not** been
compiled or validated on the n11/n12 machines in this chat. Test each
individual size and the mixed workload, first using the blocking mode and
then `nonblocking=1`, including both `rx_single_copy` states, before
drawing throughput/latency conclusions.

## n12-driven fixed-size and per-worker rate sweep

Topology: **n12 is the server (192.168.11.93)** and **n11 is the client
(192.168.11.97)**. Run `run_sweep_n12.sh` **on n12 as root**. It starts
`run_server.sh` locally and SSHes to n11 to run `run_client.sh`;
the benchmark output and per-node metrics snapshots are collected under
n12's `OUT_DIR`. SSH to n11 must be non-interactive. Both nodes must
have built the matching HomaModule and HomaKBench revision; set `CLIENT_DIR`
to the absolute path of the n11 checkout if it differs from n12.

The client binds its sockets to `src_ip=192.168.11.97` by default.
Override `src_ip` for a different testbed; the sweep forwards
`CLIENT_IP` as `src_ip` when starting the n11 client.

```sh
# From n12 (server)
cd /usr/src/HomaModule/HomaKBench

# Six workers, 1 MiB, compare RX modes at four per-worker rates.
# Aggregate offered RPC/s = 6 * (32, 64, 128, 256).
NUM_SOCKETS=6 SIZES="1048576" RX_MODES="0 1" \
  RATES="32 64 128 256" REPEATS=3 bash ./run_sweep_n12.sh

# Backward-compatible single rate (12 RPC/s per worker).
RATE=12 REPEATS=3 bash ./run_sweep_n12.sh
```

Configuration variables: `SIZES` (space-separated sizes in bytes),
`RX_MODES` (default `"0 1"`), `REPEATS`, `RATES` (space-separated
per-worker RPC/s values), `RATE` (single-rate fallback, default 12),
`NUM_SOCKETS`, `TX_ZC`, `NONBLOCKING`, `RX_BVEC`, `DURATION`,
`WARMUP`, `PORT`, `OUT_DIR`, `CLIENT_SSH`, `CLIENT_DIR`,
`CLIENT_IP`, and `SERVER_IP`.

`RATES` takes precedence over `RATE`; omitting `RATES` preserves the
existing single-rate interface. Each value is RPC/s **per worker**:
aggregate offered target is `rate * NUM_SOCKETS`. The default
`RATE=12` with `NUM_SOCKETS=16` targets 192 aggregate RPC/s.
Rate values must be unique integers in 1..1000000.
Earlier aggregate-rate benchmarks have different semantics.

Each result directory has a unique name
`rate<per_worker>_size<size_bytes>_rx<mode>_rep<repeat>`.
`config.txt` records the entire `rates_per_worker` list; `manifest.csv`
records `rate_per_worker` and `target_aggregate_rps` for each trial.
All CSV files retain rate as a dimension: `summary.csv` groups by
(size, RX mode, per-worker rate), never merges multiple offered rates.

Each case captures the client benchmark output and reads
`/proc/net/homa_metrics` on **both nodes** immediately before and after
the client run. These native Homa per-CPU counters are free-running,
so `summarize_sweep.py` first sums each counter across cores and then
subtracts the before/after snapshots. The script writes:

- `runs.csv`: every trial's P50/P99, rate per worker, aggregate offered
  target, achieved rate, skipped offered slots, error count, and validity
- `stages.csv`: per-side, per-rate RX stage ns total, count, bytes,
  average ns and ns/4KiB, plus available TX `temp[0..6]` breakdown
- `summary.csv`: per-size/per-rate median P50 and median/mean/stddev
  P99, mean achieved rate and skipped slots, RX ns/4KiB, and available
  TX average ns across valid repeats

**RX hot-path instrumentation:** The five RX stages are `add_packet`,
`copy_to_pool`, `actor_copy`, `app_copy` (KBench only), and
`skb_free`. Each stage uses `ktime_get_ns()` around the operation and
native `INC_METRIC()` to accumulate ns/count/bytes into
`struct homa_metrics`. These counters have no enable toggle and do
not calculate squared latency or standard deviations inside the hot
path. Average ns and ns/4KiB are calculated offline. `actor_copy`
includes the callback and skb-to-bvec copy when `rx_bvec=1`;
`copy_to_pool` covers the native skb-to-pool path. `app_copy`
accounts for the optional second pool-to-consumer copy.

**TX breakdown:** The original `temp[0..6]` counters in
`homa_outgoing.c` remain unchanged. `temp[0]/temp[4]` measure
message-out fill time/count, `temp[1]` packet-allocation time,
`temp[2]` Homa xmit processing time, `temp[3]` page-stash time, and
`temp[5]/temp[6]` individual xmit-data time/count. The original
TX temp fields do not provide per-size variance or a denominator
for every stage; unavailable summary values are left blank.

The native metrics include warmup, while benchmark P99 excludes warmup.
Unlike the removed `homa_tl_record()` framework, the stage counters do
not provide per-stage standard deviation or maximum latency.

The summary P99 is the **median of the per-trial P99 values**, not a
pooled P99 computed from all RPCs. P99 depends on message size, offered
rate, and the number of active sockets. A run with nonzero
`failed_rpc` is retained in `runs.csv` but excluded from the summary.

The code and scripts require **kernel compilation and runtime validation**
on n11/n12 before using the output as research measurements.
