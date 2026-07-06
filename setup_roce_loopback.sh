#!/usr/bin/env bash
#
# Per-boot setup for the SIGCOMM BlueField-3 ECN / PCC tutorial.
#
# Reserves hugepages and places each Scalable Function (SF) RoCE endpoint in its own network
# namespace, so that traffic between them is forced out onto the wire (across the p0<->p1 DAC
# loopback) where DOCA Flow on PF0 can mark it. None of this survives a reboot or power-cycle,
# so re-run this script after every boot.
#
# Prerequisites: DOCA/MFT installed under /opt/mellanox, and the two SFs already present
# (mlx5_2 on PF0, mlx5_3 on PF1 — created by the DPU at boot). See README.md.

set -euo pipefail

# --- configuration (matches README) -----------------------------------------------------------
SF0_RDMA=mlx5_2;  SF0_NETDEV=enp3s0f0s0;  NS0=ns0;  IP0=10.0.0.1/24    # receiver / server (NP)
SF1_RDMA=mlx5_3;  SF1_NETDEV=enp3s0f1s0;  NS1=ns1;  IP1=10.0.0.2/24    # sender  / client (RP)
RECEIVER_IP=10.0.0.1
UNKNOWN_MAC=12:34:56:78:9a:bc   # sender's neighbor for the receiver — intentionally NOT mlx5_2's
                                # real MAC (see README: keeps PF0's kernel FDB from delivering it
                                # unmarked; DOCA Flow rewrites it to the receiver's real MAC).

echo "== tearing down any existing ${NS0}, ${NS1} (returns their SFs to the default namespace) =="
for ns in "$NS0" "$NS1"; do
  sudo ip netns del "$ns" 2>/dev/null || true
done
sleep 1

# --- sanity: the SFs must now be visible in the default namespace ------------------------------
if ! rdma link show "${SF0_RDMA}/1" >/dev/null 2>&1 || ! rdma link show "${SF1_RDMA}/1" >/dev/null 2>&1; then
  echo "ERROR: ${SF0_RDMA} and/or ${SF1_RDMA} not found in the default namespace." >&2
  echo "       Create the SFs first (see 'mlnx-sf -a show')." >&2
  exit 1
fi

echo "== reserving 4G hugepages (DPDK / DPA need them) =="
sudo /opt/mellanox/dpdk/bin/dpdk-hugepages.py --reserve 4G

echo "== creating namespaces ${NS0}, ${NS1} =="
sudo ip netns add "$NS0"
sudo ip netns add "$NS1"

echo "== moving each SF's RDMA device + netdev into its namespace =="
# rdma runs in netns-exclusive mode, so the rdma dev and its netdev must both move.
sudo rdma dev set "$SF0_RDMA" netns "$NS0"
sudo ip link set "$SF0_NETDEV" netns "$NS0"
sudo rdma dev set "$SF1_RDMA" netns "$NS1"
sudo ip link set "$SF1_NETDEV" netns "$NS1"

echo "== configuring ${NS0} (receiver ${IP0%/*}) and ${NS1} (sender ${IP1%/*}) =="
sudo ip netns exec "$NS0" ip link set lo up
sudo ip netns exec "$NS0" ip link set "$SF0_NETDEV" up
sudo ip netns exec "$NS0" ip addr add "$IP0" dev "$SF0_NETDEV"

sudo ip netns exec "$NS1" ip link set lo up
sudo ip netns exec "$NS1" ip link set "$SF1_NETDEV" up
sudo ip netns exec "$NS1" ip addr add "$IP1" dev "$SF1_NETDEV"

echo "== pinning sender's neighbor for ${RECEIVER_IP} to unknown MAC ${UNKNOWN_MAC} =="
sudo ip netns exec "$NS1" ip neigh replace "$RECEIVER_IP" lladdr "$UNKNOWN_MAC" dev "$SF1_NETDEV" nud permanent

echo
echo "== done =="
echo "  ${NS0}:"; sudo ip netns exec "$NS0" rdma dev show
echo "  ${NS1}:"; sudo ip netns exec "$NS1" rdma dev show
echo
echo "Hugepages: $(grep HugePages_Total /proc/meminfo | awk '{print $2}') x 2M reserved."
echo "Next: build (see README), start doca-flow-ecn on PF0, then doca_pcc on PF1, then drive traffic."
