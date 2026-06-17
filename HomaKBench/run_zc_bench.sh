#!/bin/bash
# Run ZC vs non-ZC TX benchmark across multiple message sizes.
# Reloads homa.ko between each run for clean metrics.
# Prints a summary table at the end.
#
# Usage: ./run_zc_bench.sh <dst_ip> [rate] [duration] [warmup]
# Example: ./run_zc_bench.sh 192.168.11.93
#          ./run_zc_bench.sh 192.168.11.93 5000 20 5
#
# NOTE: kbench_server must already be running on the remote machine.
#       This script only manages the CLIENT side (homa.ko + kbench_client).

set -e

DST_IP=${1:?Usage: $0 dst_ip [rate] [duration] [warmup]}
RATE=${2:-10000}
DURATION=${3:-15}
WARMUP=${4:-5}
NUM_SOCKETS=1
PORT=${PORT:-5000}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOMA_DIR="$(dirname "$SCRIPT_DIR")"
TOTAL_WAIT=$((DURATION + 5))

SIZES=(4096 131072 1310720)
SIZE_NAMES=("4KB" "128KB" "1280KB")

# Storage for summary (indexed by "size_zc")
declare -A R_TPUT R_P50 R_P90 R_P99
declare -A R_FILL R_ALLOC R_XMIT R_STASH R_COUNT
declare -A R_TXLAT_P50 R_TXLAT_P90 R_TXLAT_P99

run_one() {
    local msg_size=$1
    local zc=$2
    local label=$3
    local key="${msg_size}_${zc}"

    # Reload homa.ko for clean metrics
    lsmod | grep -q '^kbench_client ' && rmmod kbench_client 2>/dev/null || true
    if lsmod | grep -q '^homa '; then
        rmmod homa
        sleep 1
    fi
    insmod "$HOMA_DIR/homa.ko"
    sleep 1

    # Reset tx_lat counter
    cat /proc/net/homa_tx_lat > /dev/null 2>&1 || true

    echo "--- $label: msg_size=$msg_size zc=$zc rate=$RATE ---"

    insmod "$SCRIPT_DIR/kbench_client.ko" \
        transport=homa \
        zc="$zc" \
        dst_ip="$DST_IP" \
        msg_size="$msg_size" \
        rate="$RATE" \
        num_sockets="$NUM_SOCKETS" \
        duration_sec="$DURATION" \
        warmup_sec="$WARMUP" \
        server_port="$PORT"

    sleep "$TOTAL_WAIT"

    # Parse kbench results
    local results=""
    if [ -f /sys/kernel/debug/homakbench/results ]; then
        results=$(cat /sys/kernel/debug/homakbench/results)
    else
        results=$(dmesg | grep "HomaKBench results" -A 10 | tail -10)
    fi
    echo "$results"

    R_TPUT[$key]=$(echo "$results" | grep -oP 'throughput: \K[0-9]+')
    R_P50[$key]=$(echo "$results" | grep -oP 'P50: \K[0-9.]+')
    R_P90[$key]=$(echo "$results" | grep -oP 'P90: \K[0-9.]+')
    R_P99[$key]=$(echo "$results" | grep -oP 'P99: \K[0-9.]+')

    # Parse tx_lat
    local txlat
    txlat=$(cat /proc/net/homa_tx_lat)
    echo ""
    echo "  TX latency (fill-to-departure, ns):"
    echo "$txlat" | sed 's/^/    /'

    R_TXLAT_P50[$key]=$(echo "$txlat" | grep -oP '^p50 \K[0-9]+' || echo "0")
    R_TXLAT_P90[$key]=$(echo "$txlat" | grep -oP '^p90 \K[0-9]+' || echo "0")
    R_TXLAT_P99[$key]=$(echo "$txlat" | grep -oP '^p99 \K[0-9]+' || echo "0")

    # Sum temp[] across all cores
    local t0=0 t1=0 t2=0 t3=0 t4=0
    while IFS=' ' read -r name val _; do
        case "$name" in
            temp0) t0=$((t0 + val)) ;;
            temp1) t1=$((t1 + val)) ;;
            temp2) t2=$((t2 + val)) ;;
            temp3) t3=$((t3 + val)) ;;
            temp4) t4=$((t4 + val)) ;;
        esac
    done < <(grep '^temp[0-4] ' /proc/net/homa_metrics)

    R_COUNT[$key]=$t4
    if [ "$t4" -gt 0 ]; then
        R_FILL[$key]=$((t0 / t4))
        R_ALLOC[$key]=$((t1 / t4))
        R_XMIT[$key]=$((t2 / t4))
        R_STASH[$key]=$((t3 / t4))
        echo ""
        echo "  temp[] avg (ns/call, $t4 samples):"
        echo "    fill=${R_FILL[$key]}  alloc=${R_ALLOC[$key]}  xmit=${R_XMIT[$key]}  stash=${R_STASH[$key]}"
    else
        R_FILL[$key]=0; R_ALLOC[$key]=0; R_XMIT[$key]=0; R_STASH[$key]=0
        echo "  temp[]: (no samples)"
    fi

    rmmod kbench_client 2>/dev/null || true
    echo ""
}

echo "========================================"
echo " ZC vs Non-ZC TX Benchmark"
echo " dst=$DST_IP rate=$RATE duration=${DURATION}s"
echo "========================================"
echo ""

for i in "${!SIZES[@]}"; do
    size=${SIZES[$i]}
    name=${SIZE_NAMES[$i]}
    run_one "$size" 0 "$name non-ZC"
    run_one "$size" 1 "$name ZC"
done

# --- Summary Table ---
echo ""
echo "=============================================================================="
echo " SUMMARY"
echo "=============================================================================="
printf "%-8s %-6s %7s %9s %9s %9s %8s %8s %8s %8s\n" \
    "Size" "Mode" "Tput" "P50(us)" "P90(us)" "P99(us)" \
    "fill" "alloc" "xmit" "stash"
printf "%-8s %-6s %7s %9s %9s %9s %8s %8s %8s %8s\n" \
    "----" "----" "-------" "---------" "---------" "---------" \
    "--------" "--------" "--------" "--------"

for i in "${!SIZES[@]}"; do
    size=${SIZES[$i]}
    name=${SIZE_NAMES[$i]}
    for zc in 0 1; do
        key="${size}_${zc}"
        if [ "$zc" -eq 0 ]; then mode="copy"; else mode="ZC"; fi
        printf "%-8s %-6s %5s/s %7s us %7s us %7s us %6s ns %6s ns %6s ns %6s ns\n" \
            "$name" "$mode" \
            "${R_TPUT[$key]:-?}" \
            "${R_P50[$key]:-?}" "${R_P90[$key]:-?}" "${R_P99[$key]:-?}" \
            "${R_FILL[$key]:-?}" "${R_ALLOC[$key]:-?}" "${R_XMIT[$key]:-?}" "${R_STASH[$key]:-?}"
    done
done

echo ""
echo "  fill/alloc/xmit/stash = avg ns/call from temp[] (homa_message_out_fill breakdown)"
echo "  P50/P90/P99 = end-to-end RPC latency from kbench_client"
echo "=============================================================================="
