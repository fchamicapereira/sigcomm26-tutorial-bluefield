#!/usr/bin/env bash

NETNS="ns1"            # network namespace holding the sender SF (client side)
RDMA_DEV="mlx5_3"      # RDMA device to drive: PF1's SF (the sender)
GID="1"                # -x: RoCEv2 GID index on this SF
INTERVAL_SEC="1"       # -D: seconds between throughput reports (reporting period, not a duration)

# Tells the tool to use the RDMA Connection Manager (RDMA_CM) to establish the initial connection
# between the client and the server, rather than the default out-of-band TCP connection.
# REQUIRED for the DOCA PCC exercise.
# It binds the QP to algo slot 0; optional for the ECN-only test.
# Set to "" to disable.
USE_RDMA_CM="1"

SERVER_IP="10.0.0.1"   # receiver's IP (the server, in ns0) — positional arg makes this the client

# stdbuf -oL because ib_write_bw block-buffers stdout whenever it is not a TTY: redirect this
# script to a file or a pipe without it and nothing appears until 4 KB has piled up, which for a
# once-a-second report is minutes. It has to sit INSIDE the sudo, since sudo strips the LD_PRELOAD
# that stdbuf works through. Same reason and same placement as in benchmark.sh.
sudo ip netns exec "$NETNS" \
    stdbuf -oL ib_write_bw \
    -d "$RDMA_DEV" \
    -x "$GID" \
    -F \
    ${USE_RDMA_CM:+-R} \
    "$SERVER_IP" \
    --report_gbits \
    --run_infinitely \
    -D "$INTERVAL_SEC"
