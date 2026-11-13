#!/bin/bash
# Usage: ./run_client.sh <transport> <zc> <dst_ip> <msg_size> <rate_per_worker> <num_sockets> [extra_params]
# Example: ./run_client.sh homa 0 192.168.11.93 4096 10000 4
#          ./run_client.sh homa 1 192.168.11.93 65536 50000 8
#          ./run_client.sh tcp 0 192.168.11.93 4096 10000 4

set -e

TRANSPORT=${1:?Usage: $0 transport zc dst_ip msg_size rate num_sockets}
ZC=${2:?}
DST_IP=${3:?}
MSG_SIZE=${4:?}
RATE=${5:?}
NUM_SOCKETS=${6:?}
shift 6

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOMA_DIR="$(dirname "$SCRIPT_DIR")"
DURATION=${DURATION:-30}
WARMUP=${WARMUP:-5}
PORT=${PORT:-5000}
SIM_APP_COPY=${SIM_APP_COPY:-0}

# Ensure homa.ko is loaded (only for Homa transport).
if [ "$TRANSPORT" = "homa" ] && ! lsmod | grep -q '^homa '; then
    echo "Loading homa.ko..."
    insmod "$HOMA_DIR/homa.ko"
fi

# Remove old client module if loaded.
lsmod | grep -q '^kbench_client ' && rmmod kbench_client

TOTAL_WAIT=$((DURATION + 12))
echo "Starting $TRANSPORT client: zc=$ZC dst=$DST_IP msg=$MSG_SIZE rate_per_worker=$RATE workers=$NUM_SOCKETS aggregate_target=$((RATE * NUM_SOCKETS)) RPC/s"
echo "Duration: ${DURATION}s (warmup: ${WARMUP}s). Results in ~${TOTAL_WAIT}s."

insmod "$SCRIPT_DIR/kbench_client.ko" \
    transport="$TRANSPORT" \
    zc="$ZC" \
    dst_ip="$DST_IP" \
    msg_size="$MSG_SIZE" \
    rate="$RATE" \
    num_sockets="$NUM_SOCKETS" \
    simulate_app_copy="$SIM_APP_COPY" \
    duration_sec="$DURATION" \
    warmup_sec="$WARMUP" \
    server_port="$PORT" \
    "$@"

echo "Waiting for completed experiment results (up to ${TOTAL_WAIT}s)..."
RESULTS_PATH=/sys/kernel/debug/homakbench/results
RESULTS=""
COMPLETE=0
for ((elapsed=0; elapsed<TOTAL_WAIT; elapsed++)); do
    # debugfs st_size can be 0. Mixed-size percentiles are written
    # AFTER the throughput line; wait until their counts add up to the
    # global latency sample count before unloading the kernel module.
    RESULTS="$(cat "$RESULTS_PATH" 2>/dev/null || true)"
    if [[ "$RESULTS" == *"HomaKBench results:"* &&
          "$RESULTS" == *"throughput:"* ]]; then
        if [[ "$RESULTS" == *"workload=blksw6"* ]]; then
            completed="$(printf '%s\n' "$RESULTS" |
                sed -n -E 's/^[[:space:]]*completed RPCs: ([0-9]+)$/\1/p')"
            class_total="$(printf '%s\n' "$RESULTS" |
                awk '$2 == "KiB:" && $3 ~ /^n=[0-9]+$/ {
                    sub(/^n=/, "", $3); total += $3
                } END {print total+0}')"
            if [[ -n "$completed" && "$completed" != 0 &&
                  "$completed" == "$class_total" ]]; then
                COMPLETE=1
                break
            fi
        else
            COMPLETE=1
            break
        fi
    fi
    sleep 1
done
if [[ "$COMPLETE" != 1 ]]; then
    echo "ERROR: no finalized KBench results after ${TOTAL_WAIT}s" >&2
    rmmod kbench_client 2>/dev/null || true
    exit 1
fi

echo ""
echo "=== Results ==="
printf '%s\n' "$RESULTS"
if [[ -n "${RESULT_FILE:-}" ]]; then
    printf '%s\n' "$RESULTS" > "$RESULT_FILE"
fi

echo ""
echo "Unloading client module..."
rmmod kbench_client 2>/dev/null || true
