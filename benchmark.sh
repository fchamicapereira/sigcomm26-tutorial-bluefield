#!/usr/bin/env bash
#
# Launches the RoCE server + client together and shows a live ttyplot chart of the
# client's (sender's) throughput. Both are stopped together on exit (Ctrl-C).
# Requires ./setup_ttyplot.sh to have been run once (builds ./ttyplot/ttyplot).
#
set -uo pipefail

SERVER_NETNS="ns0";  SERVER_DEV="mlx5_2"                       # receiver SF (PF0)
CLIENT_NETNS="ns1";  CLIENT_DEV="mlx5_3";  SERVER_IP="10.0.0.1" # sender SF (PF1)
GID="1"                # -x: RoCEv2 GID index on both SFs
INTERVAL_SEC="1"       # -D: seconds between throughput reports

# Tells the tool to use the RDMA Connection Manager (RDMA_CM) to establish the initial connection
# between the client and the server, rather than the default out-of-band TCP connection.
# REQUIRED for the DOCA PCC exercise.
# It binds the QP to algo slot 0; optional for the ECN-only test.
# Set to "" to disable.
USE_RDMA_CM="1"

CLEANED_UP=""
cleanup() {
  [ -n "$CLEANED_UP" ] && return
  CLEANED_UP=1
  echo "Stopping server..."
  # Match by argv, not $!: sudo runs with use_pty on this box, so a backgrounded
  # `sudo ... &` job's PID is sudo's own pty-monitor process, not ib_write_bw itself —
  # killing/waiting on that PID doesn't reliably reach or reap the real command.
  sudo pkill -INT -f "ib_write_bw -d $SERVER_DEV" 2>/dev/null
  sleep 1
}
# EXIT alone is enough: bash runs it on normal completion AND after an uncaught INT/TERM.
trap cleanup EXIT

echo "Starting server..."
sudo ip netns exec "$SERVER_NETNS" \
    ib_write_bw \
    -d "$SERVER_DEV" \
    -x "$GID" \
    -F \
    ${USE_RDMA_CM:+-R} \
    --report_gbits \
    --run_infinitely \
    -D "$INTERVAL_SEC" \
    > /dev/null 2>&1 &

sleep 2 # let the server reach "waiting for client"

echo "Starting client (live plot)..."
sudo ip netns exec "$CLIENT_NETNS" \
    stdbuf -oL ib_write_bw \
    -d "$CLIENT_DEV" \
    -x "$GID" \
    -F \
    ${USE_RDMA_CM:+-R} \
    "$SERVER_IP" \
    --report_gbits \
    --run_infinitely \
    -D "$INTERVAL_SEC" \
| awk '$1 == "65536" && NF == 5 { print $4; fflush(); }' \
| ./ttyplot/ttyplot -t "RoCE throughput (client -> server)" -u "Gb/s"
