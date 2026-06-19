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
declare -A R_IQXMIT R_NPKTS R_SKBFREE

# Format ns with auto unit: >=10000ns → X.Xus, otherwise Xns
fmt_ns() {
    local v=$1
    if [ "$v" -ge 10000 ]; then
        printf "%s.%sus" "$((v / 1000))" "$(( (v % 1000) / 100 ))"
    else
        printf "%sns" "$v"
    fi
}

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
    sysctl -w net.homa.hijack_tcp=1 > /dev/null

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

    # Sum temp[] across all cores + collect skb_free metrics
    local t0=0 t1=0 t2=0 t3=0 t4=0 t5=0 t6=0
    local skb_free_cyc=0 skb_free_cnt=0 cpu_khz=0

    # Get cpu_khz first (appears once, not per-core)
    cpu_khz=$(awk '/^cpu_khz /{print $2; exit}' /proc/net/homa_metrics)
    cpu_khz=${cpu_khz:-1}

    # Sum per-core metrics
    while read -r name val _; do
        case "$name" in
            temp0) t0=$((t0 + val)) ;;
            temp1) t1=$((t1 + val)) ;;
            temp2) t2=$((t2 + val)) ;;
            temp3) t3=$((t3 + val)) ;;
            temp4) t4=$((t4 + val)) ;;
            temp5) t5=$((t5 + val)) ;;
            temp6) t6=$((t6 + val)) ;;
            skb_free_cycles) skb_free_cyc=$((skb_free_cyc + val)) ;;
            skb_frees) skb_free_cnt=$((skb_free_cnt + val)) ;;
        esac
    done < <(grep -E '^(temp[0-6]|skb_free_cycles|skb_frees) ' /proc/net/homa_metrics)

    R_COUNT[$key]=$t4
    if [ "$t4" -gt 0 ] && [ "$cpu_khz" -gt 0 ]; then
        R_FILL[$key]=$((t0 / t4))
        R_ALLOC[$key]=$((t1 / t4))
        R_XMIT[$key]=$((t2 / t4))
        R_STASH[$key]=$((t3 / t4))
        R_IQXMIT[$key]=$((t5 / t4))
        R_NPKTS[$key]=$((t6 / t4))
        # cycles → ns: ns = cycles * 1000000 / cpu_khz
        local skb_free_ns
        skb_free_ns=$(echo "$skb_free_cyc * 1000000 / $cpu_khz / $t4" | bc)
        R_SKBFREE[$key]=$skb_free_ns
        echo ""
        echo "  temp[] avg (ns/call, $t4 RPCs, $t6 pkts):"
        echo "    fill=${R_FILL[$key]}  alloc=${R_ALLOC[$key]}  xmit_in_fill=${R_XMIT[$key]}  stash=${R_STASH[$key]}"
        echo "    iq_xmit/rpc=${R_IQXMIT[$key]}  pkts/rpc=${R_NPKTS[$key]}  iq_xmit/pkt=$((t5 / t6))"
        echo "    skb_free/rpc=$(fmt_ns "$skb_free_ns") ($skb_free_cnt skbs freed)"
    else
        R_FILL[$key]=0; R_ALLOC[$key]=0; R_XMIT[$key]=0; R_STASH[$key]=0
        R_IQXMIT[$key]=0; R_NPKTS[$key]=0; R_SKBFREE[$key]=0
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
printf "%-8s %-5s %7s %9s %9s %9s %9s %9s %9s %9s %5s %9s %9s %9s\n" \
    "Size" "Mode" "Tput" "txP50us" "txP90us" "txP99us" \
    "fill" "alloc" "stash" "iq_xmit" "pkts" "iq/pkt" "skb_free" "free/pkt"
printf "%s\n" "----------------------------------------------------------------------------------------------------------------------"

for i in "${!SIZES[@]}"; do
    size=${SIZES[$i]}
    name=${SIZE_NAMES[$i]}
    for zc in 0 1; do
        key="${size}_${zc}"
        if [ "$zc" -eq 0 ]; then mode="copy"; else mode="ZC"; fi
        local_p50=${R_TXLAT_P50[$key]:-0}
        local_p90=${R_TXLAT_P90[$key]:-0}
        local_p99=${R_TXLAT_P99[$key]:-0}
        npkts=${R_NPKTS[$key]:-0}
        iqxmit=${R_IQXMIT[$key]:-0}
        skbfree=${R_SKBFREE[$key]:-0}
        if [ "$npkts" -gt 0 ]; then
            iq_per_pkt=$((iqxmit / npkts))
            free_per_pkt=$((skbfree / npkts))
        else
            iq_per_pkt=0
            free_per_pkt=0
        fi
        printf "%-8s %-5s %5s/s %6s.%sus %6s.%sus %6s.%sus %9s %9s %9s %9s %5s %9s %9s %9s\n" \
            "$name" "$mode" \
            "${R_TPUT[$key]:-?}" \
            "$((local_p50 / 1000))" "$(( (local_p50 % 1000) / 100 ))" \
            "$((local_p90 / 1000))" "$(( (local_p90 % 1000) / 100 ))" \
            "$((local_p99 / 1000))" "$(( (local_p99 % 1000) / 100 ))" \
            "$(fmt_ns "${R_FILL[$key]:-0}")" "$(fmt_ns "${R_ALLOC[$key]:-0}")" \
            "$(fmt_ns "${R_STASH[$key]:-0}")" \
            "$(fmt_ns "$iqxmit")" "$npkts" "$(fmt_ns "$iq_per_pkt")" \
            "$(fmt_ns "$skbfree")" "$(fmt_ns "$free_per_pkt")"
    done
done

echo ""
echo "  txP50/P90/P99 = TX-only fill-to-last-departure (homa_tx_lat)"
echo "  fill/alloc/stash = per-RPC avg (homa_message_out_fill breakdown)"
echo "  iq_xmit = per-RPC total ip_queue_xmit (all paths) | iq/pkt = per-GSO-pkt"
echo "  skb_free = per-RPC SKB release cost (put_page + consume_skb) | free/pkt = per-GSO-pkt"
echo "=============================================================================="
