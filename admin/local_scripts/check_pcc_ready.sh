#!/usr/bin/env bash
#
# Target-side: is this machine's firmware NV-config ready for the DOCA PCC exercise (Part IV)?
#
# Two knobs decide it (see the README's "Firmware NV-config (PCC prerequisite)" section):
#
#   USER_PROGRAMMABLE_CC must be 1 — otherwise doca_pcc fails with
#                                    "PCC CONFIG object is not supported on this device".
#   DPA_AUTHENTICATION   must be 0 — otherwise the firmware only runs signed DPA images and
#                                    rejects a locally dpacc-built one (syndrome 0x8f333).
#
# What matters is the *Current* (live) column, not Next Boot: `mlxconfig set` only stages a value,
# and it goes live when the DPU re-reads NV-config after actually losing power. So this reports
# three distinct states, because the work each one implies is very different:
#
#   ready        Current is already right on both. Nothing to do.
#   power-cycle  Current is wrong but Next Boot is right: already staged, so a full power-off is
#                all that is left. A `reboot`, a BMC warm reset and `chassis power cycle` do NOT
#                count — the BlueField-3 has its own power rail that survives them. Power off,
#                wait ~30 s for the rail to drain, power on.
#   configure    Next Boot is wrong too: somebody has to run `mlxconfig set` first, and then
#                power-cycle. This is the only state that needs a decision from an admin.
#
# Read-only: it runs `mlxconfig -e q` and nothing else. It never stages or resets anything.
#
# fleet.py pipes this over stdin (`ssh <host> bash -s -- <args>`), so it MUST stay self-contained.
# It is also meant to be runnable by hand on any machine the fleet tool cannot reach.
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
#
# Usage:
#   ./check_pcc_ready.sh          # human-readable report
#   ./check_pcc_ready.sh --emit   # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: check_pcc_ready.sh [--emit]

Report whether USER_PROGRAMMABLE_CC and DPA_AUTHENTICATION are live, staged, or unset, and
therefore whether this machine is ready for the DOCA PCC exercise. Reads only.

      --emit    print '@@key=value' lines (upcc, dpa, verdict, device) instead of the human
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

command -v mlxconfig >/dev/null 2>&1 || die "mlxconfig not found (MFT is not installed)"

# mlxconfig reads PCI config space, so it needs root even to query.
if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	sudo -n true 2>/dev/null || die "passwordless sudo is required (logged in as $(id -un))"
	SUDO=(sudo -n)
fi

# --- pick a device -------------------------------------------------------------------------
# The mst device is what the README uses, but it only exists once `mst start` has run, and
# starting it is a state change we have no business making during a read-only check. mlxconfig
# takes a plain PCI address too, so fall back to the one behind the first IB device.
#
# Either way this is PF0 (mt41692_pciconf0, not ...0.1): NV-config is per-device, and both PFs
# report the same values.
pick_device() {
	local dev
	for dev in /dev/mst/mt*_pciconf0; do
		[ -e "$dev" ] && { printf '%s\n' "$dev"; return 0; }
	done

	local ibdev
	for ibdev in /sys/class/infiniband/*/device; do
		[ -e "$ibdev" ] || continue
		dev=$(basename "$(readlink -f "$ibdev")")
		# 0000:03:00.1 is PF1; keep looking for function 0.
		case "$dev" in
			*.0) printf '%s\n' "$dev"; return 0 ;;
		esac
	done
	return 1
}

DEVICE=$(pick_device) || die "no BlueField device found (no /dev/mst/mt*_pciconf0, no mlx5 PCI device)"

query=$("${SUDO[@]}" mlxconfig -d "$DEVICE" -e q 2>&1) \
	|| die "mlxconfig -d $DEVICE -e q failed: $(printf '%s' "$query" | tail -1)"

# --- read the two knobs ------------------------------------------------------------------------
# With -e the last three fields of a knob's line are Default, Current and Next Boot. A leading
# '*' marks a non-default value, which is why the fields are counted from the right.
field() {
	printf '%s\n' "$query" | awk -v knob="$1" -v col="$2" '
		$0 ~ knob { print (col == "current") ? $(NF - 1) : $NF; exit }
	'
}

# "True(1)" / "False(0)" -> 1 / 0; anything already bare is passed through.
number() {
	local raw="$1"
	if [[ "$raw" =~ \(([0-9]+)\) ]]; then
		printf '%s\n' "${BASH_REMATCH[1]}"
	else
		printf '%s\n' "$raw"
	fi
}

upcc_current=$(number "$(field USER_PROGRAMMABLE_CC current)")
upcc_next=$(number "$(field USER_PROGRAMMABLE_CC next)")
dpa_current=$(number "$(field DPA_AUTHENTICATION current)")
dpa_next=$(number "$(field DPA_AUTHENTICATION next)")

for v in "$upcc_current" "$upcc_next" "$dpa_current" "$dpa_next"; do
	[ -n "$v" ] || die "could not parse USER_PROGRAMMABLE_CC / DPA_AUTHENTICATION out of mlxconfig on $DEVICE"
done

# --- verdict -----------------------------------------------------------------------------------
if [ "$upcc_current" = "1" ] && [ "$dpa_current" = "0" ]; then
	verdict="ready"
	subject="both knobs live"
elif [ "$upcc_next" = "1" ] && [ "$dpa_next" = "0" ]; then
	verdict="power-cycle"
	subject="staged; needs a full power-off (reboot is not enough)"
else
	verdict="configure"
	subject="mlxconfig set USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0, then power-cycle"
fi

# Current alone when nothing is pending, 'current→next' when a power-cycle would change it.
show() {
	if [ "$1" = "$2" ]; then printf '%s\n' "$1"; else printf '%s→%s\n' "$1" "$2"; fi
}

if [ "$EMIT" -eq 1 ]; then
	emit upcc    "$(show "$upcc_current" "$upcc_next")"
	emit dpa     "$(show "$dpa_current" "$dpa_next")"
	emit verdict "$verdict"
	emit device  "$DEVICE"
	emit subject "$subject"
	exit 0
fi

echo "device: $DEVICE"
printf '%s\n' "$query" | grep -E 'USER_PROGRAMMABLE_CC|DPA_AUTHENTICATION' || true
echo
echo "USER_PROGRAMMABLE_CC  current=$upcc_current next=$upcc_next  (must be 1)"
echo "DPA_AUTHENTICATION    current=$dpa_current next=$dpa_next  (must be 0)"
echo
echo "verdict: $verdict — $subject"

case "$verdict" in
	power-cycle)
		echo
		echo "A reboot, a BMC power reset and 'chassis power cycle' all leave Current unchanged:"
		echo "the BlueField-3 rail survives them. Power off, wait ~30s, power on:"
		echo "  ipmitool -H <bmc> -U <user> -P <pass> chassis power off && sleep 30 && \\"
		echo "  ipmitool -H <bmc> -U <user> -P <pass> chassis power on"
		;;
	configure)
		echo
		echo "  sudo mlxconfig -y -d $DEVICE set USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0"
		echo "then power-cycle as above."
		;;
esac
