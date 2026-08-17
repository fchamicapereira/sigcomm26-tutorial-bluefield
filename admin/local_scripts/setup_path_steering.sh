#!/usr/bin/env bash
# Extend setup_roce_loopback.sh with the second PF0 receiver used by PCC path steering.
# DESTRUCTIVE: the base setup deletes every OVS bridge and SF on PF0/PF1.
set -euo pipefail

REPO="${TUTORIAL_REPO:-/home/s26t/sigcomm26-tutorial-bluefield}"
BASE="$REPO/admin/local_scripts/setup_roce_loopback.sh"
PF0_PCI="${PF0_PCI:-0000:03:00.0}"

while [ $# -gt 0 ]; do
  case "$1" in
    --repo) REPO="$2"; BASE="$2/admin/local_scripts/setup_roce_loopback.sh"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
NS_PATH0=ns0
NS_SENDER=ns1
NS_PATH1=ns0_1
PATH0_NETDEV=enp3s0f0s0
SENDER_NETDEV=enp3s0f1s0
PATH1_REP=en3f0pf0sf4
PATH1_NETDEV=enp3s0f0s4
PATH1_RDMA=mlx5_4
PATH1_IP=10.0.0.11/24
BR0=ovsbr1
BR1=ovsbr2
OF_COOKIE=0x5160c26

if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; else SUDO=""; fi
die() { echo "ERROR: $*" >&2; exit 1; }

[ -x "$BASE" ] || die "$BASE is missing or not executable"
echo "== installing the base path0/sender topology =="
$SUDO "$BASE"

# setup_roce_loopback creates PF0 SF0 first and PF1 SF0 second. Creating PF0
# SF4 now deliberately gives it the next RDMA index, mlx5_4.
base_mac=$(cat /sys/class/net/p0/address)
IFS=: read -r m0 m1 m2 m3 m4 m5 <<<"$base_mac"
first=$(printf '%02x' "$((0x$m0 | 0x02))")
last=$(printf '%02x' "$((0x$m5 ^ 0x04))")
PATH1_MAC="$first:$m1:$m2:$m3:$m4:$last"
PATH0_MAC=$($SUDO ip -n "$NS_PATH0" link show "$PATH0_NETDEV" | awk '/link\/ether/ {print $2}')
[ "$PATH1_MAC" != "$base_mac" ] && [ "$PATH1_MAC" != "$PATH0_MAC" ] ||
  die "derived path1 MAC $PATH1_MAC is not unique"

echo "== creating PF0 sf4 for path1 (${PATH1_MAC}) =="
$SUDO mlnx-sf -a create -d "$PF0_PCI" -n 4 -m "$PATH1_MAC" -t ||
  die "could not create PF0 sf4"

for _ in $(seq 1 30); do
  [ -e "/sys/class/net/$PATH1_REP" ] &&
  [ -e "/sys/class/net/$PATH1_NETDEV" ] &&
  rdma link show "$PATH1_RDMA/1" >/dev/null 2>&1 && break
  sleep 1
done
[ -e "/sys/class/net/$PATH1_REP" ] || die "$PATH1_REP did not appear"
[ -e "/sys/class/net/$PATH1_NETDEV" ] || die "$PATH1_NETDEV did not appear"
rdma link show "$PATH1_RDMA/1" >/dev/null 2>&1 || die "$PATH1_RDMA did not appear"
parent=$(basename "$(dirname "$(readlink -f "/sys/class/net/$PATH1_NETDEV/device")")")
[ "$parent" = "$PF0_PCI" ] || die "$PATH1_NETDEV belongs to $parent, expected $PF0_PCI"

echo "== attaching path1 representor to ${BR0} =="
$SUDO ovs-vsctl --may-exist add-port "$BR0" "$PATH1_REP"
$SUDO ip link set "$PATH1_REP" up

echo "== creating ${NS_PATH1} (${PATH1_IP}) =="
$SUDO ip netns del "$NS_PATH1" 2>/dev/null || true
$SUDO ip netns add "$NS_PATH1"
$SUDO rdma dev set "$PATH1_RDMA" netns "$NS_PATH1"
$SUDO ip link set "$PATH1_NETDEV" netns "$NS_PATH1"
$SUDO ip netns exec "$NS_PATH1" ip link set lo up
$SUDO ip netns exec "$NS_PATH1" ip link set "$PATH1_NETDEV" up
$SUDO ip netns exec "$NS_PATH1" ip addr add "$PATH1_IP" dev "$PATH1_NETDEV"

SENDER_MAC=$($SUDO ip -n "$NS_SENDER" link show "$SENDER_NETDEV" | awk '/link\/ether/ {print $2}')
[ -n "$SENDER_MAC" ] || die "could not read sender MAC"

# Avoid an ARP warm-up dependency. The sender needs both receiver MACs, and
# both receivers need the sender for CM replies, ACKs and CNPs.
echo "== pinning dual-path neighbors =="
$SUDO ip netns exec "$NS_SENDER" ip neigh replace 10.0.0.1 lladdr "$PATH0_MAC" dev "$SENDER_NETDEV" nud permanent
$SUDO ip netns exec "$NS_SENDER" ip neigh replace 10.0.0.11 lladdr "$PATH1_MAC" dev "$SENDER_NETDEV" nud permanent
$SUDO ip netns exec "$NS_PATH0" ip neigh replace 10.0.0.2 lladdr "$SENDER_MAC" dev "$PATH0_NETDEV" nud permanent
$SUDO ip netns exec "$NS_PATH1" ip neigh replace 10.0.0.2 lladdr "$SENDER_MAC" dev "$PATH1_NETDEV" nud permanent

# setup_roce_loopback already pins path0 and sender on BR1. Add path1 to the
# same physical output so neither receiver destination is flood-forwarded.
P1_OFPORT=$($SUDO ovs-vsctl get Interface p1 ofport)
$SUDO ovs-ofctl add-flow "$BR1" \
  "cookie=${OF_COOKIE},priority=200,dl_dst=${PATH1_MAC},actions=output:${P1_OFPORT}"

echo "== verifying path-steering topology =="
fail=0
check() {
  if [ "$2" = "$3" ]; then echo "   ok    $1"; else echo "   FAIL  $1: expected '$2', got '$3'" >&2; fail=1; fi
}
check "$NS_PATH1 address" "${PATH1_IP%/*}" \
  "$($SUDO ip -n "$NS_PATH1" -4 -br addr show "$PATH1_NETDEV" | awk '{print $3}' | cut -d/ -f1)"
check "$NS_PATH1 RDMA device" "$PATH1_RDMA" \
  "$($SUDO ip netns exec "$NS_PATH1" rdma dev show 2>/dev/null | awk 'NR==1{sub(/:$/,"",$2); print $2}')"
check "$NS_PATH1 MAC" "$PATH1_MAC" \
  "$($SUDO ip -n "$NS_PATH1" link show "$PATH1_NETDEV" | awk '/link\/ether/ {print $2}')"
check "$NS_SENDER path1 neighbor" "$PATH1_MAC" \
  "$($SUDO ip netns exec "$NS_SENDER" ip neigh show 10.0.0.11 | awk '{for(i=1;i<NF;i++) if($i=="lladdr") print $(i+1)}')"
check "$BR0 path1 member" "1" \
  "$($SUDO ovs-vsctl list-ports "$BR0" | grep -cx "$PATH1_REP" || true)"
check "$BR1 pinned flows" "3" \
  "$($SUDO ovs-ofctl dump-flows "$BR1" "cookie=${OF_COOKIE}/-1" | grep -c cookie= || true)"
guid=$($SUDO ip netns exec "$NS_PATH1" rdma dev show "$PATH1_RDMA" 2>/dev/null |
  awk '{for(i=1;i<NF;i++) if($i=="node_guid") print $(i+1)}')
[ -n "$guid" ] && [ "$guid" != 0000:0000:0000:0000 ] || { echo "   FAIL  $PATH1_RDMA node GUID is invalid" >&2; fail=1; }
[ "$fail" -eq 0 ] || die "path-steering setup did not converge"

echo
echo "== path-steering topology ready =="
echo "  path0: $NS_PATH0 / mlx5_2 / 10.0.0.1 / pf0sf0"
echo "  sender: $NS_SENDER / mlx5_3 / 10.0.0.2 / pf1sf0"
echo "  path1: $NS_PATH1 / $PATH1_RDMA / ${PATH1_IP%/*} / pf0sf4"

