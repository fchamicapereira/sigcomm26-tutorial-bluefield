#!/usr/bin/env bash

NETNS="ns0"            # network namespace holding the receiver SF (server side)
RDMA_DEV="mlx5_2"      # RDMA device to drive: PF0's SF (the receiver)
GID="1"                # -x: RoCEv2 GID index on this SF
INTERVAL_SEC="1"       # -D: seconds between throughput reports (reporting period, not a duration)

# Tells the tool to use the RDMA Connection Manager (RDMA_CM) to establish the initial connection
# between the client and the server, rather than the default out-of-band TCP connection.
# REQUIRED for the DOCA PCC exercise.
# It binds the QP to algo slot 0; optional for the ECN-only test.
# Set to "" to disable.
USE_RDMA_CM="1"

sudo ip netns exec "$NETNS" \
    ib_write_bw \
    -d "$RDMA_DEV" \
    -x "$GID" \
    -F \
    ${USE_RDMA_CM:+-R} \
    --report_gbits \
    --run_infinitely \
    -D "$INTERVAL_SEC"
