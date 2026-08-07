#!/usr/bin/env bash
#
# Build the tutorial container image FOR ONE DOCA VERSION and drop into an interactive shell
# inside it, with the DPU hardware exposed so the FULL tutorial — including
# setup_roce_loopback.sh — runs INSIDE the container:
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
#   sudo ./admin/local_scripts/setup_roce_loopback.sh   # hugepages + ns0/ns1 + SF placement
#   sudo ./build/doca-flow/doca_flow_ecn_pcap &   # start the ECN marker on PF0
#   ./run_server.sh                            # receiver (ns0) — calls sudo internally
#   ./run_client.sh                            # sender  (ns1), in another shell / tmux
#
# The DOCA version is a REQUIRED argument. Every doca-*/ directory has its own Dockerfile with its
# own base image and its own sources, and the DPU only works with the release it is running — a
# default here would silently build against the wrong SDK and fail at runtime, far from the cause.
# `admin/fleet.py doca` prints which version each machine needs.
#
# Usage:
#   ./run_container.sh 2.9                  # build for DOCA 2.9, then interactive bash
#   ./run_container.sh doca-2.9             # same (the 'doca-' prefix is optional)
#   ./run_container.sh 2.9 <cmd> [args…]    # build, then run <cmd> instead of bash
#   IMAGE=myname ./run_container.sh 2.9     # override the tag (default: sigcomm26-tutorial:2.9)
#
set -euo pipefail

# Build context = this script's directory (the repo root), so `docker build` works no matter where
# the script is invoked from, and so the image gets BOTH the version's sources and the shared
# scripts (admin/local_scripts/, docker-entrypoint.sh, run_*.sh) the Dockerfile needs.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

available() {
  local d
  for d in "$SCRIPT_DIR"/doca-*/; do
    [ -f "$d/Dockerfile" ] && basename "$d" | sed 's/^doca-//'
  done | sort | paste -sd' ' -
}

usage() {
  cat >&2 <<EOF
Usage: ./run_container.sh <doca-version> [command [args…]]

The DOCA version is required — it selects doca-<version>/Dockerfile and its base image.
Available in this checkout: $(available)

  ./run_container.sh 2.9                 # interactive shell
  ./run_container.sh 2.9 ./run_server.sh # run one command instead
EOF
}

if [ $# -eq 0 ]; then
  echo "ERROR: no DOCA version given." >&2
  usage
  exit 2
fi

case "$1" in
  -h|--help) usage; exit 0 ;;
esac

VERSION="${1#doca-}"   # accept both '2.9' and 'doca-2.9'
shift
DOCA_DIR="doca-${VERSION}"

if [ ! -f "$SCRIPT_DIR/$DOCA_DIR/Dockerfile" ]; then
  echo "ERROR: no such DOCA version '$VERSION' ($DOCA_DIR/Dockerfile does not exist)." >&2
  usage
  exit 2
fi

IMAGE="${IMAGE:-sigcomm26-tutorial:${VERSION}}"

if [[ ! -e /dev/infiniband ]]; then
  echo "WARNING: /dev/infiniband not found on the host — no RDMA devices to expose." >&2
fi
# The setup script reserves hugepages itself (inside the container); we only need the host's
# OVS daemon reachable so its ovs-vsctl step can ensure the persistent ovsbr1/ovsbr2 bridges.
if [[ ! -S /run/openvswitch/db.sock ]]; then
  echo "WARNING: /run/openvswitch/db.sock not found — host OVS may not be running." >&2
  echo "         setup_roce_loopback.sh's ovs-vsctl step needs the host OVS daemon." >&2
fi

echo ">> Building ${IMAGE} from ${DOCA_DIR}/Dockerfile ..."
# DOCA_DIR is passed as a build arg as well as selecting the Dockerfile: the context is the repo
# root, so the Dockerfile has to be told which version directory to build inside it.
docker build -t "${IMAGE}" \
  -f "${SCRIPT_DIR}/${DOCA_DIR}/Dockerfile" \
  --build-arg "DOCA_DIR=${DOCA_DIR}" \
  "${SCRIPT_DIR}"

echo ">> Starting ${IMAGE} ..."
exec docker run --rm -it \
  --privileged \
  --net=host \
  -v /dev/infiniband:/dev/infiniband \
  -v /dev/hugepages:/dev/hugepages \
  -v /run/openvswitch:/run/openvswitch \
  "${IMAGE}" "$@"
