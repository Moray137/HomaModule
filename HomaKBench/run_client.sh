#!/bin/bash
# Usage: ./run_client.sh <transport> <zc> <dst_ip> <msg_size> <rate> <num_sockets> [extra_params]
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

TOTAL_WAIT=$((DURATION + 5))
echo "Starting $TRANSPORT client: zc=$ZC dst=$DST_IP msg=$MSG_SIZE rate=$RATE socks=$NUM_SOCKETS"
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

echo "Waiting ${TOTAL_WAIT}s for experiment to complete..."
sleep "$TOTAL_WAIT"

echo ""
echo "=== Results ==="
if [ -f /sys/kernel/debug/homakbench/results ]; then
    cat /sys/kernel/debug/homakbench/results
else
    dmesg | grep "HomaKBench results" -A 20 | tail -10
fi

echo ""
echo "Unloading client module..."
rmmod kbench_client 2>/dev/null || true
