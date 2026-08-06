#!/usr/bin/env bash
#
# Restore the NVIDIA lab BlueField-3 ("testbed B", see README "Reference testbeds") to the
# configuration it ships with, undoing everything ../setup_roce_loopback.sh does.
#
# ../setup_roce_loopback.sh deliberately wipes the DPU's SFs and OVS bridges and rebuilds the layout
# the tutorial needs (one SF per PF, sfnum 0, ovsbr1/ovsbr2). On a DPU we own that is fine. The
# NVIDIA lab DPU is *not* ours: it ships staged for NVIDIA's own DOCA walkthrough, which its
# internal documentation describes as
#
#   - two SFs, BOTH on PF0: sfnum 2 and sfnum 3
#       pci/0000:03:00.0/229408  en3f0pf0sf2  enp3s0f0s2  mlx5_2  HWADDR 44:38:39:00:00:02
#       pci/0000:03:00.0/229409  en3f0pf0sf3  enp3s0f0s3  mlx5_3  HWADDR 44:38:39:00:00:03
#       both: trust on, roce true
#   - no SF on PF1; p1 and pf1hpf attached to no bridge
#   - two OVS bridges bridging the host to the wire through a DOCA app:
#       host_br: pf0hpf + en3f0pf0sf2
#       wire_br: p0     + en3f0pf0sf3
#
# and that is exactly the state this script recreates. Run it when you are done with the tutorial
# on that box, or any time you need to hand it back.
#
# Everything here is runtime state — SFs and OVS bridges do not survive a reboot — so a power-cycle
# also restores the box. This script just avoids having to reboot.

set -euo pipefail

PF0_PCI="${PF0_PCI:-0000:03:00.0}"
PF1_PCI="${PF1_PCI:-0000:03:00.1}"

if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; else SUDO=""; fi

echo "== removing tutorial network namespaces =="
for ns in ns0 ns1; do
  $SUDO ip netns del "$ns" 2>/dev/null || true
done
sleep 1

echo "== removing all OVS bridges =="
for br in $($SUDO ovs-vsctl list-br 2>/dev/null || true); do
  echo "   del-br ${br}"
  $SUDO ovs-vsctl --if-exists del-br "$br"
done

echo "== deleting all SFs on ${PF0_PCI} and ${PF1_PCI} =="
# mlnx-sf prints "SF Index: pci/<pci>/<id>"; delete by that index.
for idx in $($SUDO mlnx-sf -a show 2>/dev/null | awk '/^SF Index:/ {print $3}'); do
  case "$idx" in
    pci/${PF0_PCI}/*|pci/${PF1_PCI}/*)
      echo "   delete ${idx}"
      $SUDO mlnx-sf -a delete -i "$idx" || true
      ;;
  esac
done
sleep 2

echo "== recreating NVIDIA's two SFs on PF0 (sfnum 2 and 3) =="
$SUDO mlnx-sf -a create -d "$PF0_PCI" -n 2 -m 44:38:39:00:00:02 -t
$SUDO mlnx-sf -a create -d "$PF0_PCI" -n 3 -m 44:38:39:00:00:03 -t
sleep 3

echo "== bringing interfaces up =="
for i in p0 p1 pf0hpf pf1hpf en3f0pf0sf2 en3f0pf0sf3 enp3s0f0s2 enp3s0f0s3; do
  $SUDO ip link set "$i" up 2>/dev/null || echo "   WARNING: ${i} not present" >&2
done

echo "== recreating host_br (pf0hpf + en3f0pf0sf2) and wire_br (p0 + en3f0pf0sf3) =="
$SUDO ovs-vsctl --may-exist add-br host_br
$SUDO ovs-vsctl --may-exist add-port host_br pf0hpf
$SUDO ovs-vsctl --may-exist add-port host_br en3f0pf0sf2

$SUDO ovs-vsctl --may-exist add-br wire_br
$SUDO ovs-vsctl --may-exist add-port wire_br p0
$SUDO ovs-vsctl --may-exist add-port wire_br en3f0pf0sf3

$SUDO ip link set host_br up
$SUDO ip link set wire_br up

# --- verify ------------------------------------------------------------------------------------
echo
echo "== verifying =="
fail=0
check() { # <description> <expected> <actual>
  if [ "$2" = "$3" ]; then
    echo "   ok    $1"
  else
    echo "   FAIL  $1: expected '$2', got '$3'" >&2
    fail=1
  fi
}

have() { [ -e "/sys/class/net/$1" ] && echo "$1"; }

check "sf2 representor" "en3f0pf0sf2" "$(have en3f0pf0sf2)"
check "sf3 representor" "en3f0pf0sf3" "$(have en3f0pf0sf3)"
check "sf2 netdev"      "enp3s0f0s2"  "$(have enp3s0f0s2)"
check "sf3 netdev"      "enp3s0f0s3"  "$(have enp3s0f0s3)"
check "no SF on PF1"    ""            "$($SUDO mlnx-sf -a show 2>/dev/null | awk '/^SF Index:/ {print $3}' | grep "pci/${PF1_PCI}/" || true)"
check "host_br members" "en3f0pf0sf2 pf0hpf" \
      "$($SUDO ovs-vsctl list-ports host_br 2>/dev/null | grep -v '^host_br$' | sort | tr '\n' ' ' | sed 's/ $//')"
check "wire_br members" "en3f0pf0sf3 p0" \
      "$($SUDO ovs-vsctl list-ports wire_br 2>/dev/null | grep -v '^wire_br$' | sort | tr '\n' ' ' | sed 's/ $//')"
check "p1 unbridged"    ""            "$($SUDO ovs-vsctl port-to-br p1 2>/dev/null || true)"
check "pf1hpf unbridged" ""           "$($SUDO ovs-vsctl port-to-br pf1hpf 2>/dev/null || true)"

echo
echo "== resulting SF layout =="
$SUDO mlnx-sf -a show
echo "== resulting OVS layout =="
$SUDO ovs-vsctl show

if [ "$fail" -ne 0 ]; then
  echo >&2
  echo "ERROR: the DPU was NOT fully restored — see the FAIL lines above." >&2
  echo "       A reboot restores it unconditionally (SFs and OVS bridges are runtime state)." >&2
  exit 1
fi

echo
echo "== done: DPU restored to NVIDIA's original configuration =="
