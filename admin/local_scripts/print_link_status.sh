#!/usr/bin/env bash
#
# Report each BlueField uplink port: device names (PCIe address and /dev/mst path), link state,
# speed and loopback mode.
#
# This is mlxlink's "Operational Info" block, per port, in one line each. Loopback mode is in here
# because a port left in loopback looks perfectly healthy to `ip link` and `ethtool` while
# silently not being on the wire at all — the kind of thing that costs an afternoon.
#
# Ports are discovered from sysfs (phys_port_name = p0, p1, ...) rather than from `mst status`, and
# mlxlink is addressed by PCI address rather than by /dev/mst/*, so this never needs `mst start` —
# loading the mst kernel modules is a state change a read-only check has no business making.
#
# fleet.py pipes this over stdin (`ssh <host> bash -s -- <args>`), so it MUST stay self-contained.
# It is also meant to be runnable by hand on any machine the fleet tool cannot reach.
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
#
# Usage:
#   ./print_link_status.sh          # human-readable report, all ports
#   ./print_link_status.sh --emit   # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: print_link_status.sh [--emit]

Report every BlueField uplink port: PCIe address, /dev/mst device, link state, speed, width,
FEC, loopback mode and auto-negotiation, as mlxlink sees them. Reads only.

      --emit    print '@@key=value' lines (pcie, mst, portN, loopback, ports) instead of the human
                report, which is what admin/fleet.py parses
EOF
}

EMIT=0
for arg in "$@"; do
	case "$arg" in
		--emit)    EMIT=1 ;;
		-h|--help) usage; exit 0 ;;
		*)         echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

emit() { printf '@@%s=%s\n' "$1" "$2"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

command -v mlxlink >/dev/null 2>&1 || die "mlxlink not found (MFT is not installed)"

# mlxlink reads PCI config space, so it needs root even to query.
if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	sudo -n true 2>/dev/null || die "passwordless sudo is required (logged in as $(id -un))"
	SUDO=(sudo -n)
fi

# --- find the uplink ports ----------------------------------------------------------------------
# On a switchdev BlueField every function exposes several netdevs (the uplink, the host PF
# representor, SF representors). Only the uplink's phys_port_name is a bare 'pN', which is what
# distinguishes it from pf0hpf / en3f0pf0sf0 and friends.
ports=()   # "netdev pci" pairs, ordered by netdev name
for netdev in /sys/class/net/*; do
	name=$(basename "$netdev")
	[ -r "$netdev/phys_port_name" ] || continue
	case "$(cat "$netdev/phys_port_name" 2>/dev/null || true)" in
		p[0-9]*) ;;
		*) continue ;;
	esac
	[ -e "$netdev/device" ] || continue
	pci=$(basename "$(readlink -f "$netdev/device")")
	ports+=("$name $pci")
done

[ ${#ports[@]} -gt 0 ] || die "no BlueField uplink ports found (no netdev with phys_port_name=pN)"

# --- mst device names ----------------------------------------------------------------------------
# Derived rather than read from `mst status`, so this still reports them when mst has not been
# started (and so it never has to start it). MFT names a device mt<device-id>_pciconf<N>, where
# <device-id> is the PCI device id in DECIMAL — BlueField-3 is 0xa2dc = 41692, hence
# mt41692_pciconf0 — <N> counts the physical devices, and extra functions of the same device get a
# '.<func>' suffix: mt41692_pciconf0 is PF0, mt41692_pciconf0.1 is PF1.
slots=()
for entry in "${ports[@]}"; do
	pci="${entry#* }"
	slot="${pci%.*}"
	case " ${slots[*]-} " in *" $slot "*) ;; *) slots+=("$slot") ;; esac
done

mst_name_for() { # $1 = PCI address, e.g. 0000:03:00.1
	local pci="$1" slot func devid dec i=0 index=0
	slot="${pci%.*}"; func="${pci##*.}"
	for s in "${slots[@]}"; do
		[ "$s" = "$slot" ] && index=$i
		i=$((i + 1))
	done
	devid=$(cat "/sys/bus/pci/devices/$pci/device" 2>/dev/null || echo 0)
	dec=$(printf '%d' "$devid" 2>/dev/null || echo 0)
	if [ "$func" = "0" ]; then
		printf 'mt%s_pciconf%s\n' "$dec" "$index"
	else
		printf 'mt%s_pciconf%s.%s\n' "$dec" "$index" "$func"
	fi
}

# If /dev/mst exists at all, mst has been started and the paths below are real files.
MST_STARTED=no
[ -d /dev/mst ] && MST_STARTED=yes

# mlxlink colourises its output; strip the escapes before parsing or every value carries them.
strip_ansi() { sed -e 's/\x1b\[[0-9;]*m//g'; }

# "Label   : value" -> value, trimmed. Labels are matched at line start to avoid the Troubleshooting
# and Module Info blocks further down, which reuse some of the same words.
field() {
	printf '%s\n' "$2" | awk -v key="$1" '
		index($0, key) == 1 {
			pos = index($0, ":")
			if (pos == 0) next
			val = substr($0, pos + 1)
			gsub(/^[ \t]+|[ \t]+$/, "", val)
			print val
			exit
		}'
}

pcie_list=()
mst_list=()
loopbacks=()
summaries=()
idx=0

for entry in "${ports[@]}"; do
	name="${entry%% *}"
	pci="${entry#* }"
	short="${pci#0000:}"   # 0000:03:00.0 -> 03:00.0, which is what mst status prints too

	if ! out=$("${SUDO[@]}" mlxlink -d "$pci" -p 1 2>&1 | strip_ansi); then
		state="unreadable"; phys="-"; speed="-"; width="-"; fec="-"; lb="-"; an="-"
	else
		state=$(field "State" "$out");            state="${state:-?}"
		phys=$(field "Physical state" "$out");    phys="${phys:-?}"
		speed=$(field "Speed" "$out");            speed="${speed:-?}"
		width=$(field "Width" "$out");            width="${width:-?}"
		fec=$(field "FEC" "$out");                fec="${fec:-?}"
		lb=$(field "Loopback Mode" "$out");       lb="${lb:-?}"
		an=$(field "Auto Negotiation" "$out");    an="${an:-?}"
	fi

	mst=$(mst_name_for "$pci")
	pcie_list+=("$short")
	mst_list+=("$mst")
	# Only a non-default loopback is worth a column; "No Loopback" is the normal case.
	case "$lb" in
		"No Loopback"|"") ;;
		*) loopbacks+=("$name:$lb") ;;
	esac

	# A port that is not Active is more usefully described by its physical state (Polling,
	# Disabled, LinkUp) than by repeating "Down".
	if [ "$state" = "Active" ]; then
		summaries+=("$name=$state $speed")
	else
		summaries+=("$name=$state ($phys)")
	fi

	if [ "$EMIT" -eq 1 ]; then
		emit "port$idx" "$name $state $speed $([ "$lb" = "No Loopback" ] || echo "lb:$lb")"
	else
		echo "$name  ($short)"
		if [ -e "/dev/mst/$mst" ]; then
			mst_shown="/dev/mst/$mst"
		else
			mst_shown="/dev/mst/$mst  (not present — mst has not been started)"
		fi
		printf '  %-18s %s\n' "mst device:" "$mst_shown" "state:" "$state" \
		                      "physical state:" "$phys" "speed:" "$speed" \
		                      "width:" "$width" "FEC:" "$fec" "loopback:" "$lb" \
		                      "auto negotiation:" "$an"
		echo
	fi
	idx=$((idx + 1))
done

join() { local IFS=,; if [ $# -eq 0 ]; then echo "-"; else echo "$*"; fi; }

if [ "$EMIT" -eq 1 ]; then
	emit ports    "${#ports[@]}"
	emit pcie     "$(join "${pcie_list[@]}")"
	emit mst      "$(join "${mst_list[@]}")"
	emit mst_started "$MST_STARTED"
	emit loopback "$(join "${loopbacks[@]}")"
	emit summary  "$(join "${summaries[@]}")"
	exit 0
fi

echo "ports: ${#ports[@]}   pcie: $(join "${pcie_list[@]}")   mst: $(join "${mst_list[@]}") (under /dev/mst/)"
if [ ${#loopbacks[@]} -gt 0 ]; then
	echo "LOOPBACK ACTIVE: ${loopbacks[*]} — this port is not on the wire"
fi
