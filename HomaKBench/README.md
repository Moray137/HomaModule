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

The current client has one outstanding request **per socket**, not sixteen
outstanding requests on one socket. Set `num_sockets=16` to allow up to
sixteen concurrent RPCs. `rate` is the offered RPC/s target. When all
available socket slots are busy, the pacer counts `skipped_busy_total`
instead of accumulating an unbounded backlog. Total offered/skipped counts
include warmup; throughput and latency samples exclude warmup.

For the six sizes individually, use `workload=fixed` and the six values
`4096`, `8192`, `12288`, `16384`, `131072`, `1048576`.

## Independent TX and RX configuration

Use `tx_zc=0|1`, `rx_single_copy=0|1`, and
`nonblocking=0|1` on **both** client and server.
`tx_zc=1` uses MSG_SPLICE_PAGES and `rx_single_copy=1` registers
Homa's full-message skb actor. The vanilla path continues using the Homa
receive pool. The older client `zc` and server `rx_actor_used` parameters
remain supported as compatibility aliases.

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
  num_sockets=16 rate=10000 duration_sec=30 warmup_sec=5 \
  tx_zc=0 rx_single_copy=1 nonblocking=1
cat /sys/kernel/debug/homakbench/results
```

For an end-to-end pool-copy baseline that includes the former second copy,
run `rx_single_copy=0 simulate_app_copy=1` on **both** the client and
the server. Server-side `simulate_app_copy=1` copies pool bytes into
a worker-local buffer, approximating the old Target pool-to-SG copy.
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

## n12-driven repeated fixed-size sweep

The script `run_sweep_n12.sh` runs on **n12 as root** and SSHes to
`root@n11` (change `SERVER_SSH` if necessary). It uses the existing
`run_server.sh` and `run_client.sh` runners, which must be built on
their respective machines. Both sides should run HomaModule from the
same `rx-side-zc-impl-reverse-walk` revision. SSH must be
non-interactive; configure `SERVER_DIR` if the two checkouts live in
different locations.

```sh
cd HomaKBench

# Default: six fixed sizes; 3 repeats; compare RX vanilla and single-copy,
# TX ZC disabled, MSG_DONTWAIT enabled; 16 independent sockets.
SERVER_SSH=root@n11 SERVER_IP=192.168.11.93 \
  SERVER_DIR=/root/HomaModule/HomaKBench \
  RATE=2000 REPEATS=3 bash ./run_sweep_n12.sh

# For five repetitions and another offered rate:
REPEATS=5 RATE=5000 bash ./run_sweep_n12.sh
```

Configuration variables: `SIZES` (space-separated sizes in bytes),
`RX_MODES` (default `"0 1"`), `REPEATS`, `RATE`, `NUM_SOCKETS`,
`TX_ZC`, `NONBLOCKING`, `RX_BVEC`, `DURATION`, `WARMUP`, `PORT`,
`OUT_DIR`, `SERVER_SSH`, `SERVER_DIR`, and `SERVER_IP`.

Each case captures the client benchmark output and
`/sys/kernel/debug/homa/transport_stats` on **both nodes**, resetting
both counters just before starting the client. The script writes:

- `runs.csv`: every trial's P50/P99, achieved rate, skipped offered slots,
  error count, and validity
- `stages.csv`: per-side per-stage timing (including `ns_per_4k`)
- `summary.csv`: per-size median/mean/stddev of trial P99 and mean
  per-stage ns/4KiB across valid repeats

`rx_single_copy=1 rx_bvec=1` (default) makes the actor copy skb data
into page-backed `bio_vec` storage using an `iov_iter`. This is
**SG-like storage**, not a real NVMe `scatterlist` or command SG. Set
`RX_BVEC=0` to use the earlier linear `app_buf` actor.
`actor_copy` measures the Homa-to-actor callback, including the bvec
copy in this mode; `copy_to_pool` measures native skb-to-pool copy;
`app_copy` measures the optional second pool-to-application copy.
`add_packet` and `skb_free` are also measured. Stage statistics
include warmup, while benchmark P99 excludes warmup.

The summary P99 is the **median of the per-trial P99 values**, not a
pooled P99 computed from all RPCs. P99 depends on message size, offered
rate, and the number of active sockets. A run with nonzero
`failed_rpc` is retained in `runs.csv` but excluded from the summary.

The code and scripts require **kernel compilation and runtime validation**
on n11/n12 before using the output as research measurements.
