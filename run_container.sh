#!/usr/bin/env bash
#
# Build the tutorial container image and drop into an interactive shell inside it, with the
# DPU hardware exposed so the FULL tutorial — including setup/setup_roce_loopback.sh — runs
# INSIDE the container:
#
#   --privileged                     full device access (PCI, mlx5 bifurcated driver)
#   -v /dev/infiniband               RDMA / uverbs devices (RoCE, ib_write_bw)
#   -v /dev/hugepages                DPDK / DPA hugepage-backed memory
#   -v /run/openvswitch              host OVS socket, so the setup script's `ovs-vsctl` can
#                                    (idempotently) ensure the host's ovsbr1/ovsbr2 bridges exist
#   --net=host                       operate on the host's PFs / SFs (shared network namespace)
#   --rm                             container is discarded on exit
#
# The container runs as the 'ubuntu' user with passwordless sudo, so every tutorial script works
# unchanged. ns0/ns1 are created by the setup script INSIDE the container — they live in the
# container's own network namespace and are torn down with it, so nothing leaks onto the host.
#
# Typical flow, all inside the shell this drops you into:
#   sudo ./setup/setup_roce_loopback.sh        # hugepages + ns0/ns1 + SF placement
#   sudo ./build/doca-flow/doca_flow_ecn &     # start the ECN marker on PF0
#   ./run_server.sh                            # receiver (ns0) — calls sudo internally
#   ./run_client.sh                            # sender  (ns1), in another shell / tmux
#
# Usage:
#   ./run_container.sh                 # build, then interactive bash
#   ./run_container.sh <cmd> [args…]   # build, then run <cmd> instead of bash
#   IMAGE=myname ./run_container.sh    # override the image tag (default: sigcomm26-tutorial)
#
set -euo pipefail

IMAGE="${IMAGE:-sigcomm26-tutorial}"

# Build context = this script's directory (the repo root), so `docker build` works
# no matter where the script is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -e /dev/infiniband ]]; then
  echo "WARNING: /dev/infiniband not found on the host — no RDMA devices to expose." >&2
fi
# The setup script reserves hugepages itself (inside the container); we only need the host's
# OVS daemon reachable so its ovs-vsctl step can ensure the persistent ovsbr1/ovsbr2 bridges.
if [[ ! -S /run/openvswitch/db.sock ]]; then
  echo "WARNING: /run/openvswitch/db.sock not found — host OVS may not be running." >&2
  echo "         setup_roce_loopback.sh's ovs-vsctl step needs the host OVS daemon." >&2
fi

echo ">> Building ${IMAGE} ..."
docker build -t "${IMAGE}" "${SCRIPT_DIR}"

echo ">> Starting ${IMAGE} ..."
exec docker run --rm -it \
  --privileged \
  --net=host \
  -v /dev/infiniband:/dev/infiniband \
  -v /dev/hugepages:/dev/hugepages \
  -v /run/openvswitch:/run/openvswitch \
  "${IMAGE}" "$@"
