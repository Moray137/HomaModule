#!/usr/bin/env bash
# Run on n12 as root. SSH to n11 must work without interactive prompts.
# Each trial uses one fixed Homa message size; request and reply sizes match.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOMA_DIR="$(dirname "$HERE")"
SERVER_SSH="${SERVER_SSH:-root@n11}"
SERVER_DIR="${SERVER_DIR:-$HERE}"
SERVER_IP="${SERVER_IP:-192.168.11.93}"
PORT="${PORT:-5000}"
SIZES="${SIZES:-4096 8192 12288 16384 131072 1048576}"
RX_MODES="${RX_MODES:-0 1}"
REPEATS="${REPEATS:-3}"
RATE="${RATE:-2000}"
NUM_SOCKETS="${NUM_SOCKETS:-16}"
DURATION="${DURATION:-30}"
WARMUP="${WARMUP:-5}"
TX_ZC="${TX_ZC:-0}"
RX_BVEC="${RX_BVEC:-1}"
NONBLOCKING="${NONBLOCKING:-1}"
OUT_DIR="${OUT_DIR:-$HERE/results_$(date +%Y%m%d_%H%M%S)}"

if (( EUID != 0 )); then
    echo "Run as root on n12 (insmod/debugfs)." >&2
    exit 1
fi
if (( REPEATS < 1 )); then
    echo "REPEATS must be >= 1" >&2
    exit 1
fi
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

remote() {
    local quoted
    printf -v quoted '%q' "$1"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$SERVER_SSH" "bash -lc $quoted"
}
cleanup() {
    remote 'rmmod kbench_server 2>/dev/null || true' >/dev/null 2>&1 || true
}
trap cleanup EXIT

# The local Homa module must exist before stats can be reset.
if ! lsmod | grep -q '^homa '; then
    insmod "$HOMA_DIR/homa.ko"
fi
if [[ ! -r /proc/net/homa_metrics ]]; then
    echo "Missing /proc/net/homa_metrics (is homa.ko loaded?)" >&2
    exit 1
fi
printf 'Preflight: server=%s, server_dir=%s, server_ip=%s\n' \
    "$SERVER_SSH" "$SERVER_DIR" "$SERVER_IP"
printf -v remote_check 'test -f %q && test -f %q' \
    "$SERVER_DIR/run_server.sh" "$SERVER_DIR/kbench_server.ko"
if ! remote "$remote_check"; then
    echo "ERROR: Cannot find required KBench files on $SERVER_SSH in $SERVER_DIR." >&2
    echo "Expected: $SERVER_DIR/run_server.sh and $SERVER_DIR/kbench_server.ko" >&2
    echo "Check SERVER_DIR (remote path), SSH access, and whether KBench was built on n11." >&2
    exit 1
fi

cat > "$OUT_DIR/config.txt" <<EOF
host=n12
server_ssh=$SERVER_SSH
server_dir=$SERVER_DIR
server_ip=$SERVER_IP
sizes=$SIZES
rx_modes=$RX_MODES
repeats=$REPEATS
rate=$RATE
num_sockets=$NUM_SOCKETS
duration=$DURATION
warmup=$WARMUP
tx_zc=$TX_ZC
rx_bvec=$RX_BVEC
nonblocking=$NONBLOCKING
EOF
printf 'size_bytes,rx_single_copy,repeat,client_result,client_before,client_after,server_before,server_after\n' > "$OUT_DIR/manifest.csv"

for rx in $RX_MODES; do
    if [[ "$rx" != 0 && "$rx" != 1 ]]; then
        echo "RX_MODES entries must be 0 or 1" >&2
        exit 1
    fi
    # Vanilla must pay for the second pool -> consumer copy.
    if [[ "$rx" == 0 ]]; then sim_copy=1; else sim_copy=0; fi
    for size in $SIZES; do
        for ((rep=1; rep<=REPEATS; rep++)); do
            name="size${size}_rx${rx}_rep${rep}"
            case_dir="$OUT_DIR/$name"
            mkdir -p "$case_dir"
            echo "=== $name (TX ZC=$TX_ZC, RX bvec=$RX_BVEC, nonblock=$NONBLOCKING) ==="

            # run_server accepts the original 4 positional args and now
            # forwards additional module parameters.
            printf -v start_server \
              'cd %q && bash ./run_server.sh homa %q %q %q tx_zc=%q rx_single_copy=%q rx_bvec=%q nonblocking=%q simulate_app_copy=%q' \
              "$SERVER_DIR" "$rx" "$SERVER_IP" "$PORT" \
              "$TX_ZC" "$rx" "$RX_BVEC" "$NONBLOCKING" "$sim_copy"
            remote "$start_server" > "$case_dir/server_start.log" 2>&1

            # Homa metrics are free-running. Snapshot both endpoints
            # before/after; summarize_sweep.py subtracts per-CPU totals.
            # Counter deltas include warmup, unlike sampled RTTs.
            cat /proc/net/homa_metrics > "$case_dir/client.before"
            remote 'cat /proc/net/homa_metrics' > "$case_dir/server.before"

            if ! (cd "$HERE" && \
                DURATION="$DURATION" WARMUP="$WARMUP" PORT="$PORT" \
                SIM_APP_COPY="$sim_copy" RESULT_FILE="$case_dir/client.result" \
                bash ./run_client.sh homa "$TX_ZC" "$SERVER_IP" "$size" \
                  "$RATE" "$NUM_SOCKETS" \
                  "tx_zc=$TX_ZC" "rx_single_copy=$rx" \
                  "rx_bvec=$RX_BVEC" "nonblocking=$NONBLOCKING") \
                > "$case_dir/client.log" 2>&1; then
                echo "Client run failed: $case_dir/client.log" >&2
                exit 1
            fi
            cat /proc/net/homa_metrics > "$case_dir/client.after"
            remote 'cat /proc/net/homa_metrics' > "$case_dir/server.after"
            remote 'rmmod kbench_server' > "$case_dir/server_stop.log" 2>&1

            if [[ ! -s "$case_dir/client.result" ]]; then
                echo "Missing result in $name" >&2
                exit 1
            fi
            printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
               "$size" "$rx" "$rep" \
               "$name/client.result" \
               "$name/client.before" "$name/client.after" \
               "$name/server.before" "$name/server.after" \
               >> "$OUT_DIR/manifest.csv"
        done
    done
done

python3 "$HERE/summarize_sweep.py" "$OUT_DIR"
echo "Results: $OUT_DIR/runs.csv, $OUT_DIR/stages.csv, $OUT_DIR/summary.csv"
