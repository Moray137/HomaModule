#!/usr/bin/env bash
# Run on server n12 (192.168.11.93) as root; SSH to client n11 (192.168.11.97).
# Fixed-size and blksw6 mixed workload; request and reply sizes match.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOMA_DIR="$(dirname "$HERE")"
CLIENT_SSH="${CLIENT_SSH:-root@192.168.11.97}"
CLIENT_DIR="${CLIENT_DIR:-$HERE}"
CLIENT_IP="${CLIENT_IP:-192.168.11.97}"
SERVER_IP="${SERVER_IP:-192.168.11.93}"
PORT="${PORT:-5000}"
SIZES="${SIZES:-4096 8192 12288 16384 131072 1048576}"
WORKLOAD="${WORKLOAD:-fixed}"
BLKSW_WEIGHTS="${BLKSW_WEIGHTS:-10,57,10,10,8,5}"
RX_MODES="${RX_MODES:-0 1}"
REPEATS="${REPEATS:-3}"
RATE="${RATE:-12}"
RATES="${RATES:-$RATE}"  # Space-separated per-worker RPC/s; RATE remains a single-rate fallback.
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
if [[ ! "$NUM_SOCKETS" =~ ^[1-9][0-9]*$ ]] ||
   (( NUM_SOCKETS > 64 )); then
    echo "NUM_SOCKETS must be 1..64." >&2
    exit 1
fi
# Keep the fixed-size loop unchanged; blksw6 needs exactly one trial
# per (rate, RX mode, repeat), regardless of the SIZES setting.
case "$WORKLOAD" in
    fixed)
        read -r -a CASE_SIZES <<< "$SIZES"
        if (( ${#CASE_SIZES[@]} == 0 )); then
            echo "SIZES must contain at least one message size." >&2
            exit 1
        fi
        for size in "${CASE_SIZES[@]}"; do
            if [[ ! "$size" =~ ^[1-9][0-9]*$ ]]; then
                echo "Invalid SIZES element: $size" >&2
                exit 1
            fi
        done
        ;;
    blksw6)
        IFS=',' read -r -a weights <<< "$BLKSW_WEIGHTS"
        if (( ${#weights[@]} != 6 )); then
            echo "BLKSW_WEIGHTS needs six comma-separated percentages." >&2
            exit 1
        fi
        total_weight=0
        for weight in "${weights[@]}"; do
            if [[ ! "$weight" =~ ^(0|[1-9][0-9]*)$ ]]; then
                echo "Invalid blksw6 weight: $weight" >&2
                exit 1
            fi
            total_weight=$((total_weight + 10#$weight))
        done
        if (( total_weight != 100 )); then
            echo "BLKSW_WEIGHTS must sum to 100 (got $total_weight)." >&2
            exit 1
        fi
        CASE_SIZES=(4096)  # Unused fixed_msg_size parameter must be positive.
        ;;
    *)
        echo "WORKLOAD must be fixed or blksw6 (got '$WORKLOAD')." >&2
        exit 1
        ;;
esac
# Parse all rate values before starting any modules or experiments.
read -r -a RATE_VALUES <<< "$RATES"
if (( ${#RATE_VALUES[@]} == 0 )); then
    echo "RATES must contain at least one per-worker RPC/s value." >&2
    exit 1
fi
declare -A seen_rates=()
for rate in "${RATE_VALUES[@]}"; do
    if [[ ! "$rate" =~ ^[1-9][0-9]*$ ]] ||
       (( ${#rate} > 7 )) ||
       (( rate > 1000000 )); then
        echo "Invalid per-worker rate: '$rate' (expected integer 1..1000000)." >&2
        exit 1
    fi
    if [[ -n "${seen_rates[$rate]:-}" ]]; then
        echo "Duplicate rate '$rate' in RATES; each rate must appear once." >&2
        exit 1
    fi
    seen_rates[$rate]=1
    echo "KBench offered load: ${rate} RPC/s/worker x ${NUM_SOCKETS} workers = $((rate * NUM_SOCKETS)) RPC/s aggregate"
done
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

remote() {
    local quoted
    printf -v quoted '%q' "$1"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_SSH" "bash -lc $quoted"
}
client_result_tmp=""
cleanup() {
    rmmod kbench_server 2>/dev/null || true
    if [[ -n "$client_result_tmp" ]]; then
        local quoted_tmp
        printf -v quoted_tmp '%q' "$client_result_tmp"
        remote "rm -f $quoted_tmp" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# The local Homa module must be loaded for server-side metrics.
if ! lsmod | grep -q '^homa '; then
    insmod "$HOMA_DIR/homa.ko"
fi
if [[ ! -r /proc/net/homa_metrics ]]; then
    echo "Missing /proc/net/homa_metrics (is homa.ko loaded?)" >&2
    exit 1
fi
printf 'Preflight: server=n12 (%s), client=%s (%s), client_dir=%s\n' \
    "$SERVER_IP" "$CLIENT_SSH" "$CLIENT_IP" "$CLIENT_DIR"
if [[ ! -f "$HERE/run_server.sh" || ! -f "$HERE/kbench_server.ko" ]]; then
    echo "ERROR: n12 server needs $HERE/run_server.sh and $HERE/kbench_server.ko" >&2
    echo "Build HomaKBench on n12 first." >&2
    exit 1
fi
printf -v remote_check 'test -f %q && test -f %q' \
    "$CLIENT_DIR/run_client.sh" "$CLIENT_DIR/kbench_client.ko"
if ! remote "$remote_check"; then
    echo "ERROR: Cannot find client files on $CLIENT_SSH in $CLIENT_DIR." >&2
    echo "Expected: $CLIENT_DIR/run_client.sh and $CLIENT_DIR/kbench_client.ko" >&2
    echo "Check CLIENT_DIR, SSH access, and whether HomaKBench was built on n11." >&2
    exit 1
fi
# Ensure n11 has a loaded Homa module before taking metrics snapshots.
printf -v client_homa_check \
    'if ! lsmod | grep -q "^homa "; then insmod %q || exit 1; fi; test -r /proc/net/homa_metrics' \
    "$CLIENT_DIR/../homa.ko"
if ! remote "$client_homa_check"; then
    echo "ERROR: Cannot load/read Homa metrics on client $CLIENT_SSH." >&2
    exit 1
fi

# Record checkout identity and on-disk KBench module hashes. These are
# provenance for the files on each node, not proof of a loaded Homa build.
server_source_head="$(git -C "$HOMA_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
printf -v client_git_cmd 'git -C %q rev-parse HEAD' "$CLIENT_DIR/.."
client_source_head="$(remote "$client_git_cmd" 2>/dev/null || echo unknown)"
server_kernel="$(uname -r)"
client_kernel="$(remote 'uname -r')"
server_kbench_ko_sha256="$(sha256sum "$HERE/kbench_server.ko" | cut -d' ' -f1)"
printf -v client_hash_cmd 'sha256sum %q' "$CLIENT_DIR/kbench_client.ko"
client_kbench_ko_sha256="$(remote "$client_hash_cmd" | cut -d' ' -f1)"

cat > "$OUT_DIR/config.txt" <<EOF
host=n12
server_node=n12
server_kernel=$server_kernel
client_kernel=$client_kernel
server_checkout_head=$server_source_head
client_checkout_head=$client_source_head
server_kbench_ko_sha256=$server_kbench_ko_sha256
client_kbench_ko_sha256=$client_kbench_ko_sha256
server_ip=$SERVER_IP
client_ssh=$CLIENT_SSH
client_dir=$CLIENT_DIR
client_ip=$CLIENT_IP
workload=$WORKLOAD
sizes_fixed_only=$SIZES
blksw_weights=$BLKSW_WEIGHTS
rx_modes=$RX_MODES
repeats=$REPEATS
rates_per_worker=$RATES
num_sockets=$NUM_SOCKETS
duration=$DURATION
warmup=$WARMUP
tx_zc=$TX_ZC
rx_bvec=$RX_BVEC
client_rx_destination=$([[ "$RX_BVEC" == "1" ]] && echo bvec || echo linear)
server_rx_destination=sgl
nonblocking=$NONBLOCKING
EOF
printf 'workload,size_bytes,rx_single_copy,rate_per_worker,target_aggregate_rps,repeat,client_result,client_before,client_after,server_before,server_after\n' > "$OUT_DIR/manifest.csv"

for rate in "${RATE_VALUES[@]}"; do
for rx in $RX_MODES; do
    if [[ "$rx" != 0 && "$rx" != 1 ]]; then
        echo "RX_MODES entries must be 0 or 1" >&2
        exit 1
    fi
    # Vanilla must pay for the second pool -> consumer copy.
    if [[ "$rx" == 0 ]]; then sim_copy=1; else sim_copy=0; fi
    for size in "${CASE_SIZES[@]}"; do
        for ((rep=1; rep<=REPEATS; rep++)); do
            if [[ "$WORKLOAD" == "blksw6" ]]; then
                name="rate${rate}_blksw6_rx${rx}_rep${rep}"
                manifest_size=""
            else
                name="rate${rate}_size${size}_rx${rx}_rep${rep}"
                manifest_size="$size"
            fi
            case_dir="$OUT_DIR/$name"
            mkdir -p "$case_dir"
            echo "=== $name (aggregate target=$((rate * NUM_SOCKETS)) RPC/s, TX ZC=$TX_ZC, RX bvec=$RX_BVEC, nonblock=$NONBLOCKING) ==="

            # n12 is the server. The RX mode and app-copy baseline
            # must match on both sides of the RPC.
            if ! bash "$HERE/run_server.sh" homa "$rx" "$SERVER_IP" "$PORT" \
                    "tx_zc=$TX_ZC" "rx_single_copy=$rx" \
                    "rx_bvec=$RX_BVEC" "nonblocking=$NONBLOCKING" \
                    "simulate_app_copy=$sim_copy" \
                    > "$case_dir/server_start.log" 2>&1; then
                echo "Server start failed: $case_dir/server_start.log" >&2
                tail -n 20 "$case_dir/server_start.log" >&2
                exit 1
            fi

            # Homa metrics are free-running. Read client from n11, server
            # locally from n12. Deltas include warmup, unlike sampled RTTs.
            remote 'cat /proc/net/homa_metrics' > "$case_dir/client.before"
            cat /proc/net/homa_metrics > "$case_dir/server.before"

            # Keep the client's result on n11 temporarily, then fetch it
            # to the n12 result directory alongside both metrics snapshots.
            client_result_tmp="$(remote 'mktemp /tmp/homakbench-client.XXXXXXXX')"
            printf -v start_client \
              'cd %q && DURATION=%q WARMUP=%q PORT=%q SIM_APP_COPY=%q RESULT_FILE=%q bash ./run_client.sh homa %q %q %q %q %q tx_zc=%q rx_single_copy=%q rx_bvec=%q nonblocking=%q src_ip=%q workload=%q blksw_weights=%q' \
              "$CLIENT_DIR" "$DURATION" "$WARMUP" "$PORT" "$sim_copy" \
              "$client_result_tmp" "$TX_ZC" "$SERVER_IP" "$size" \
              "$rate" "$NUM_SOCKETS" "$TX_ZC" "$rx" "$RX_BVEC" \
              "$NONBLOCKING" "$CLIENT_IP" "$WORKLOAD" "$BLKSW_WEIGHTS"
            if ! remote "$start_client" > "$case_dir/client.log" 2>&1; then
                echo "Client run failed on n11: $case_dir/client.log" >&2
                tail -n 20 "$case_dir/client.log" >&2
                exit 1
            fi
            remote 'cat /proc/net/homa_metrics' > "$case_dir/client.after"
            cat /proc/net/homa_metrics > "$case_dir/server.after"

            printf -v quoted_tmp '%q' "$client_result_tmp"
            remote "cat $quoted_tmp" > "$case_dir/client.result"
            remote "rm -f $quoted_tmp"
            client_result_tmp=""
            rmmod kbench_server > "$case_dir/server_stop.log" 2>&1

            if [[ ! -s "$case_dir/client.result" ]]; then
                echo "Missing result in $name" >&2
                exit 1
            fi
            printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
               "$WORKLOAD" "$manifest_size" "$rx" "$rate" "$((rate * NUM_SOCKETS))" "$rep" \
               "$name/client.result" \
               "$name/client.before" "$name/client.after" \
               "$name/server.before" "$name/server.after" \
               >> "$OUT_DIR/manifest.csv"
        done
    done
done
done

python3 "$HERE/summarize_sweep.py" "$OUT_DIR"
echo "Results: $OUT_DIR/runs.csv, $OUT_DIR/stages.csv, $OUT_DIR/summary.csv, $OUT_DIR/size_runs.csv, $OUT_DIR/size_summary.csv"
