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
run `rx_single_copy=0 simulate_app_copy=1` on the client and
`rx_single_copy=0` on the server. Do **not** enable
`simulate_app_copy` for `rx_single_copy=1`.
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
