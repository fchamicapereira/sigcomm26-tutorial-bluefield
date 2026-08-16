#!/usr/bin/env bash
#
# Launches the RoCE server + client together and shows a live ttyplot chart of the
# client's (sender's) throughput. Both are stopped together on exit (Ctrl-C).
# ttyplot is optional: resolved on PATH first (the fleet installs it to /usr/local/bin via
# admin/local_scripts/install_deps.sh), then a repo-local build; absent that, plain throughput.
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

# Two independent buffering hazards sit between ib_write_bw and ttyplot; either one alone leaves
# the chart stuck on "waiting for data from stdin" until a ~4 KB buffer fills (then it jumps in
# one burst):
#   1. ib_write_bw block-buffers stdout when it's a pipe (not a TTY) -> `stdbuf -oL` below.
#   2. mawk block-buffers its INPUT. `-W interactive` makes mawk read line-by-line and write
#      unbuffered. (awk's fflush() only affects OUTPUT, so it is not enough on its own.)
# Which awk you get depends on where you run: the DPU natively has gawk, which already streams
# and prints a warning if handed -W interactive; the tutorial container (Ubuntu 22.04 minimal,
# see the Dockerfile) has mawk, which needs the flag. So detect and pass it only to mawk.
AWK=(awk)
if [ "$(basename "$(readlink -f "$(command -v awk)")")" = "mawk" ]; then
  AWK=(awk -W interactive)
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# A live chart is nice but optional. Locate ttyplot in this order:
#   1. on PATH -- the permanent install. Build it once (admin/local_scripts/setup_ttyplot.sh) and
#      `sudo install -m0755 ttyplot/ttyplot /usr/local/bin/`; then every repo/checkout finds it.
#   2. a source build in the repo -- <repo-root>/ttyplot (setup_ttyplot.sh's default) or right beside
#      this script, which is where the Dockerfiles place it.
# If ttyplot is nowhere, fall back to plain per-second throughput lines, so this script still
# launches both ends with one command everywhere.
TTYPLOT="$(command -v ttyplot || true)"
if [ -z "$TTYPLOT" ]; then
  for cand in "$SCRIPT_DIR/../ttyplot/ttyplot" "$SCRIPT_DIR/ttyplot/ttyplot"; do
    [ -x "$cand" ] && { TTYPLOT="$cand"; break; }
  done
fi
HAVE_TTYPLOT=0
if [ -n "$TTYPLOT" ] && [ -x "$TTYPLOT" ]; then
  HAVE_TTYPLOT=1
else
  echo "Note: ttyplot not found (not on PATH or in the repo) -- showing plain throughput instead of a live chart." >&2
fi

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

echo "Starting client..."
run_client() {
  sudo ip netns exec "$CLIENT_NETNS" \
      stdbuf -oL ib_write_bw \
      -d "$CLIENT_DEV" \
      -x "$GID" \
      -F \
      ${USE_RDMA_CM:+-R} \
      "$SERVER_IP" \
      --report_gbits \
      --run_infinitely \
      -D "$INTERVAL_SEC"
}

if [ "$HAVE_TTYPLOT" = 1 ]; then
  run_client \
    | "${AWK[@]}" '$1 == "65536" && NF == 5 { print $4; fflush(); }' \
    | "$TTYPLOT" -t "RoCE throughput (client -> server)" -u "Gb/s"
else
  run_client
fi
