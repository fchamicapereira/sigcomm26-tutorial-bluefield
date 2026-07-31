#!/usr/bin/env bash
#
# Per-boot setup for the SIGCOMM BlueField-3 ECN / PCC tutorial.
#
# This script does not adapt to the DPU it finds — it *imposes* a known layout and refuses to
# continue if it cannot produce it exactly. Everything downstream (the doca-flow programs, the
# run_server/run_client scripts, the README's command lines) is written against that one layout,
# so there is nothing to discover and no per-box flags to pass.
#
# It is destructive by design. It deletes EVERY OVS bridge and EVERY SF on both PFs, then builds:
#
#   SF  PF0 sfnum 0 -> representor en3f0pf0sf0, netdev enp3s0f0s0, RDMA mlx5_2   (receiver, NP)
#   SF  PF1 sfnum 0 -> representor en3f1pf1sf0, netdev enp3s0f1s0, RDMA mlx5_3   (sender,   RP)
#   OVS ovsbr1: p0 + pf0hpf + en3f0pf0sf0        (PF0 side — the "default" forwarding path)
#   OVS ovsbr2: p1 + pf1hpf + en3f1pf1sf0        (PF1 side — puts the sender's traffic on the wire)
#   NS  ns0: enp3s0f0s0 @ 10.0.0.1/24            NS ns1: enp3s0f1s0 @ 10.0.0.2/24
#
# Why one SF per PF: the sender must sit on PF1 so its traffic leaves the DPU on p1 and re-enters
# at p0, which is where DOCA Flow marks it. Both SFs on one PF would never reach the wire.
#
# Nothing here survives a reboot or power-cycle (SFs, bridges and namespaces are all runtime
# state), so re-run after every boot.
#
# On a DPU that ships staged for something else, this wipes that staging. For the NVIDIA lab DPU
# see reset_nvidia_dpu_to_original_config.sh, which puts its original layout back.
#
# Prerequisites: DOCA/MFT under /opt/mellanox, mlnx-sf on PATH, both PFs in switchdev mode.

set -euo pipefail

# --- the layout this script imposes -------------------------------------------------------------
PF0_PCI="${PF0_PCI:-0000:03:00.0}"
PF1_PCI="${PF1_PCI:-0000:03:00.1}"

SF0_REP=en3f0pf0sf0;  SF0_NETDEV=enp3s0f0s0;  SF0_RDMA=mlx5_2   # receiver / server (NP), on PF0
SF1_REP=en3f1pf1sf0;  SF1_NETDEV=enp3s0f1s0;  SF1_RDMA=mlx5_3   # sender   / client (RP), on PF1

# Pin each SF's hardware address rather than letting the firmware pick one. An SF created without
# -m can come up with hw_addr 00:00:00:00:00:00, and since the RDMA node GUID is derived from it, a
# zero there breaks RoCE connection setup — observed on PF1 of the NVIDIA lab DPU, whose PF0 has a
# MAC pool configured but whose PF1 does not.
#
# The address must NOT be a fixed constant: on a DPU whose ports are cabled into a shared fabric
# (rather than to each other) these MACs are visible to every other machine in the same broadcast
# domain, so running this tutorial on two DPUs in one lab would put duplicate MACs on the same VLAN
# and flap the switches' forwarding tables. Derive it instead from the PF uplink's burned-in
# address, which is globally unique per NIC, by setting the locally-administered bit. That keeps it
# stable across re-runs on one box and distinct across boxes.
la_mac_from() {
  local base first
  base=$(cat "/sys/class/net/$1/address") || return 1
  first=$(printf '%02x' "$(( 0x${base%%:*} | 0x02 ))")
  echo "${first}:${base#*:}"
}

NS0=ns0;  IP0=10.0.0.1/24
NS1=ns1;  IP1=10.0.0.2/24
RECEIVER_IP=${IP0%/*}

BR0=ovsbr1;  BR1=ovsbr2
OF_COOKIE=0x5160c26        # tags the flows this script installs so a re-run replaces only its own

if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; else SUDO=""; fi

die() { echo "ERROR: $*" >&2; exit 1; }

# --- 0. preconditions ---------------------------------------------------------------------------
command -v mlnx-sf  >/dev/null || die "mlnx-sf not found on PATH (install mlnx-tools / mft)."
command -v ovs-vsctl >/dev/null || die "ovs-vsctl not found on PATH."
for p in p0 p1 pf0hpf pf1hpf; do
  [ -e "/sys/class/net/$p" ] || die "uplink/representor '$p' does not exist. Is the DPU in DPU (switchdev) mode?"
done

SF0_MAC=$(la_mac_from p0) || die "could not read p0's MAC address."
SF1_MAC=$(la_mac_from p1) || die "could not read p1's MAC address."
# A burned-in vendor address never has the locally-administered bit set, so the derived address is
# always distinct from the uplink's. Check anyway rather than silently duplicating a MAC.
[ "$SF0_MAC" != "$(cat /sys/class/net/p0/address)" ] || die "derived SF0 MAC ${SF0_MAC} collides with p0's own address."
[ "$SF1_MAC" != "$(cat /sys/class/net/p1/address)" ] || die "derived SF1 MAC ${SF1_MAC} collides with p1's own address."

# --- 1. tear everything down --------------------------------------------------------------------
echo "== tearing down namespaces ${NS0}, ${NS1} (returns their SFs to the default namespace) =="
for ns in "$NS0" "$NS1"; do
  $SUDO ip netns del "$ns" 2>/dev/null || true
done
sleep 1

echo "== deleting every OVS bridge =="
for br in $($SUDO ovs-vsctl list-br 2>/dev/null || true); do
  echo "   del-br ${br}"
  $SUDO ovs-vsctl --if-exists del-br "$br"
done

echo "== deleting every SF on ${PF0_PCI} and ${PF1_PCI} =="
# An SF's RDMA device index (mlx5_N) is handed out in probe order, not derived from its sfnum, so
# the only way to land on a predictable mlx5_2/mlx5_3 is to start from zero SFs and create ours in
# a fixed order. That is also why this deletes SFs it did not create.
for idx in $($SUDO mlnx-sf -a show 2>/dev/null | awk '/^SF Index:/ {print $3}'); do
  case "$idx" in
    pci/${PF0_PCI}/*|pci/${PF1_PCI}/*)
      echo "   delete ${idx}"
      $SUDO mlnx-sf -a delete -i "$idx" || die "could not delete SF ${idx}."
      ;;
  esac
done
sleep 2

# --- 2. build the SFs ---------------------------------------------------------------------------
# PF0 first, then PF1: the RDMA indices follow creation order, giving mlx5_2 then mlx5_3.
echo "== creating one SF per PF (sfnum 0 on each) =="
echo "   receiver MAC ${SF0_MAC} (from p0), sender MAC ${SF1_MAC} (from p1)"
$SUDO mlnx-sf -a create -d "$PF0_PCI" -n 0 -m "$SF0_MAC" -t || die "could not create the SF on PF0 (${PF0_PCI})."
$SUDO mlnx-sf -a create -d "$PF1_PCI" -n 0 -m "$SF1_MAC" -t || die "could not create the SF on PF1 (${PF1_PCI}).
       If this DPU has never had an SF on PF1, check that PF1 is in switchdev mode:
         sudo devlink dev eswitch show pci/${PF1_PCI}"

echo "== waiting for the SF netdevs and RDMA devices to appear =="
for _ in $(seq 1 30); do
  if [ -e "/sys/class/net/$SF0_NETDEV" ] && [ -e "/sys/class/net/$SF1_NETDEV" ] \
     && [ -e "/sys/class/net/$SF0_REP" ] && [ -e "/sys/class/net/$SF1_REP" ] \
     && rdma link show "${SF0_RDMA}/1" >/dev/null 2>&1 \
     && rdma link show "${SF1_RDMA}/1" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

# Assert the exact layout. Everything below, and every command line in the README, depends on it.
for n in "$SF0_REP" "$SF1_REP" "$SF0_NETDEV" "$SF1_NETDEV"; do
  [ -e "/sys/class/net/$n" ] || die "expected netdev '${n}' was not created.
       Got instead:
$(ls /sys/class/net | grep -E '^(en3f|enp3s)' | sed 's/^/         /')"
done
for r in "$SF0_RDMA" "$SF1_RDMA"; do
  rdma link show "${r}/1" >/dev/null 2>&1 || die "expected RDMA device '${r}' was not created.
       Got instead:
$(rdma link show 2>/dev/null | awk '{print $2}' | sed 's|/1||' | sed 's/^/         /')
       RDMA indices are assigned in probe order. If stale SFs from an earlier run are still
       holding mlx5_2/mlx5_3, reboot the DPU and re-run this script."
done
# Confirm each SF really landed on the PF we asked for (a wrong-PF sender never reaches the wire).
sf_parent() { basename "$(dirname "$(readlink -f "/sys/class/net/$1/device")")"; }
[ "$(sf_parent "$SF0_NETDEV")" = "$PF0_PCI" ] || die "${SF0_NETDEV} is on $(sf_parent "$SF0_NETDEV"), expected PF0 ${PF0_PCI}."
[ "$(sf_parent "$SF1_NETDEV")" = "$PF1_PCI" ] || die "${SF1_NETDEV} is on $(sf_parent "$SF1_NETDEV"), expected PF1 ${PF1_PCI}."

echo "   receiver: PF0 sf0  ${SF0_NETDEV} (${SF0_RDMA})  rep ${SF0_REP}"
echo "   sender:   PF1 sf0  ${SF1_NETDEV} (${SF1_RDMA})  rep ${SF1_REP}"

# --- 3. OVS bridges -----------------------------------------------------------------------------
# One ASAP²-hardware-offloaded L2 learning bridge per PF: the "default" forwarding path referenced
# throughout the README. DOCA Flow only ever takes over PF0's FDB (fdb_def_rule_en=1 keeps PF1 on
# this path), so PF1's traffic — and PF0's, whenever no doca-flow program is running — needs these.
echo "== creating ${BR0} (PF0 side) and ${BR1} (PF1 side) =="
$SUDO ovs-vsctl add-br "$BR0"
$SUDO ovs-vsctl add-port "$BR0" p0
$SUDO ovs-vsctl add-port "$BR0" pf0hpf
$SUDO ovs-vsctl add-port "$BR0" "$SF0_REP"

$SUDO ovs-vsctl add-br "$BR1"
$SUDO ovs-vsctl add-port "$BR1" p1
$SUDO ovs-vsctl add-port "$BR1" pf1hpf
$SUDO ovs-vsctl add-port "$BR1" "$SF1_REP"

for i in p0 p1 pf0hpf pf1hpf "$SF0_REP" "$SF1_REP" "$BR0" "$BR1"; do
  $SUDO ip link set "$i" up
done

# --- 4. hugepages -------------------------------------------------------------------------------
echo "== reserving 4G hugepages (DPDK / DPA need them) =="
$SUDO /opt/mellanox/dpdk/bin/dpdk-hugepages.py --reserve 4G

# --- 5. namespaces ------------------------------------------------------------------------------
# They stop the Linux kernel from delivering 10.0.0.1 <-> 10.0.0.2 locally — both IPs sit on this
# one host, so without isolation the kernel short-circuits them and RoCE never touches the wire.
echo "== creating namespaces ${NS0}, ${NS1} =="
$SUDO ip netns add "$NS0"
$SUDO ip netns add "$NS1"

echo "== moving each SF's RDMA device + netdev into its namespace =="
# rdma runs in netns-exclusive mode, so the rdma dev and its netdev must both move.
$SUDO rdma dev set "$SF0_RDMA" netns "$NS0"
$SUDO ip link set "$SF0_NETDEV" netns "$NS0"
$SUDO rdma dev set "$SF1_RDMA" netns "$NS1"
$SUDO ip link set "$SF1_NETDEV" netns "$NS1"

echo "== configuring ${NS0} (receiver ${IP0%/*}) and ${NS1} (sender ${IP1%/*}) =="
$SUDO ip netns exec "$NS0" ip link set lo up
$SUDO ip netns exec "$NS0" ip link set "$SF0_NETDEV" up
$SUDO ip netns exec "$NS0" ip addr add "$IP0" dev "$SF0_NETDEV"

$SUDO ip netns exec "$NS1" ip link set lo up
$SUDO ip netns exec "$NS1" ip link set "$SF1_NETDEV" up
$SUDO ip netns exec "$NS1" ip addr add "$IP1" dev "$SF1_NETDEV"

RECEIVER_MAC=$($SUDO ip -n "$NS0" link show "$SF0_NETDEV" | awk '/link\/ether/ {print $2}')
SENDER_MAC=$($SUDO ip -n "$NS1" link show "$SF1_NETDEV" | awk '/link\/ether/ {print $2}')
[ -n "$RECEIVER_MAC" ] && [ -n "$SENDER_MAC" ] || die "could not read the SF MAC addresses."

echo "== pinning sender's neighbor for ${RECEIVER_IP} to ${SF0_NETDEV}'s MAC ${RECEIVER_MAC} =="
$SUDO ip netns exec "$NS1" ip neigh replace "$RECEIVER_IP" lladdr "$RECEIVER_MAC" dev "$SF1_NETDEV" nud permanent

# --- 6. keep the loopback off the flood path ----------------------------------------------------
# Left alone, ${BR1} has never seen the receiver's MAC as a *source*, so every frame the sender
# emits is unknown-unicast and gets flooded out every port, p1 included. On a DPU whose two ports
# are cabled to each other that is harmless — the only place a flooded frame can go is p0. On a DPU
# whose ports are cabled into a shared fabric it means flooding the entire VLAN at line rate, which
# is antisocial and may trip storm control. Pin both directions so the traffic is plain unicast:
#   receiver's MAC -> out p1        (sender -> wire -> p0)
#   sender's MAC   -> the sender SF (returning ACKs / CNPs)
# Everything else still falls through to NORMAL (learning) at the default priority.
echo "== pinning ${BR1} forwarding (no unknown-unicast flooding onto the wire) =="
P1_OFPORT=$($SUDO ovs-vsctl get Interface p1 ofport)
SF1_OFPORT=$($SUDO ovs-vsctl get Interface "$SF1_REP" ofport)
$SUDO ovs-ofctl del-flows "$BR1" "cookie=${OF_COOKIE}/-1"
$SUDO ovs-ofctl add-flow "$BR1" \
  "cookie=${OF_COOKIE},priority=200,dl_dst=${RECEIVER_MAC},actions=output:${P1_OFPORT}"
$SUDO ovs-ofctl add-flow "$BR1" \
  "cookie=${OF_COOKIE},priority=200,dl_dst=${SENDER_MAC},actions=output:${SF1_OFPORT}"

# --- 7. verify ----------------------------------------------------------------------------------
echo
echo "== verifying =="
fail=0
check() { if [ "$2" = "$3" ]; then echo "   ok    $1"; else echo "   FAIL  $1: expected '$2', got '$3'" >&2; fail=1; fi; }

check "${BR0} members" "en3f0pf0sf0 p0 pf0hpf" \
      "$($SUDO ovs-vsctl list-ports "$BR0" | grep -v "^${BR0}$" | sort | tr '\n' ' ' | sed 's/ $//')"
check "${BR1} members" "en3f1pf1sf0 p1 pf1hpf" \
      "$($SUDO ovs-vsctl list-ports "$BR1" | grep -v "^${BR1}$" | sort | tr '\n' ' ' | sed 's/ $//')"
check "${NS0} RDMA dev" "$SF0_RDMA" "$($SUDO ip netns exec "$NS0" rdma dev show 2>/dev/null | awk 'NR==1{sub(/:$/,"",$2); print $2}')"
check "${NS1} RDMA dev" "$SF1_RDMA" "$($SUDO ip netns exec "$NS1" rdma dev show 2>/dev/null | awk 'NR==1{sub(/:$/,"",$2); print $2}')"
check "${NS0} address"  "$RECEIVER_IP" \
      "$($SUDO ip netns exec "$NS0" ip -4 -br addr show "$SF0_NETDEV" | awk '{print $3}' | cut -d/ -f1)"
check "${NS1} address"  "${IP1%/*}" \
      "$($SUDO ip netns exec "$NS1" ip -4 -br addr show "$SF1_NETDEV" | awk '{print $3}' | cut -d/ -f1)"
check "sender neigh pinned" "$RECEIVER_MAC" \
      "$($SUDO ip netns exec "$NS1" ip neigh show "$RECEIVER_IP" | awk '{for(i=1;i<NF;i++) if($i=="lladdr") print $(i+1)}')"
check "${NS0} SF MAC" "$SF0_MAC" "$RECEIVER_MAC"
check "${NS1} SF MAC" "$SF1_MAC" "$SENDER_MAC"
# The RDMA node GUID is derived from the SF's hw_addr; a zero GUID breaks RoCE connection setup.
for pair in "${NS0}|${SF0_RDMA}" "${NS1}|${SF1_RDMA}"; do
  ns=${pair%%|*}; rd=${pair##*|}
  guid=$($SUDO ip netns exec "$ns" rdma dev show "$rd" 2>/dev/null \
         | awk '{for(i=1;i<NF;i++) if($i=="node_guid") print $(i+1)}')
  if [ -n "$guid" ] && [ "$guid" != "0000:0000:0000:0000" ]; then
    echo "   ok    ${rd} node GUID ${guid}"
  else
    echo "   FAIL  ${rd} node GUID is zero or unreadable ('${guid}') — RoCE will not connect" >&2
    fail=1
  fi
done
check "${BR1} pinned flows" "2" \
      "$($SUDO ovs-ofctl dump-flows "$BR1" "cookie=${OF_COOKIE}/-1" | grep -c cookie= || true)"
HP=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
[ "$HP" -gt 0 ] && echo "   ok    hugepages reserved (${HP} x 2M)" || { echo "   FAIL  no hugepages reserved" >&2; fail=1; }

[ "$fail" -eq 0 ] || die "setup did not converge on the expected layout — see the FAIL lines above."

echo
echo "== done =="
echo "  ${NS0} (receiver ${RECEIVER_IP}, ${RECEIVER_MAC}):"; $SUDO ip netns exec "$NS0" rdma dev show
echo "  ${NS1} (sender   ${IP1%/*}, ${SENDER_MAC}):"; $SUDO ip netns exec "$NS1" rdma dev show
echo
echo "Next: build (see README), then"
echo "    sudo ./build/doca-flow/doca_flow_ecn         # ECN marker on PF0"
echo "    ./run_server.sh                              # receiver, in ${NS0}"
echo "    ./run_client.sh                              # sender,   in ${NS1}"
