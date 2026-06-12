#!/bin/bash
# Usage: ./run_server.sh <transport> <rx_actor_used> [bind_ip] [port]
# Example: ./run_server.sh homa 1
#          ./run_server.sh tcp 0

set -e

TRANSPORT=${1:-homa}
RX_ACTOR=${2:-0}
BIND_IP=${3:-192.168.11.93}
PORT=${4:-5000}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOMA_DIR="$(dirname "$SCRIPT_DIR")"

# Ensure homa.ko is loaded (only for Homa transport).
if [ "$TRANSPORT" = "homa" ] && ! lsmod | grep -q '^homa '; then
    echo "Loading homa.ko..."
    insmod "$HOMA_DIR/homa.ko"
fi

# Remove old server module if loaded.
lsmod | grep -q '^kbench_server ' && rmmod kbench_server

echo "Starting $TRANSPORT server on $BIND_IP:$PORT (zc=$RX_ACTOR)..."
insmod "$SCRIPT_DIR/kbench_server.ko" \
    transport="$TRANSPORT" \
    rx_actor_used="$RX_ACTOR" \
    bind_ip="$BIND_IP" \
    server_port="$PORT"

echo "Server running. Use 'rmmod kbench_server' to stop."
echo "Logs: dmesg | grep kbench_server"
