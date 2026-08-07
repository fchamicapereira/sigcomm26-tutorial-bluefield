#!/usr/bin/env bash
#
# Stage the two NV-config knobs the DOCA PCC exercise needs. Run this ON THE ARM CORES.
#
#   USER_PROGRAMMABLE_CC=1   enables the programmable-CC/PCC object
#   DPA_AUTHENTICATION=0     lets the firmware run locally dpacc-built (unsigned) DPA images
#
# Setting them is only half the job: `mlxconfig set` writes the Next Boot column, and the value
# goes live when the DPU actually loses power. A `reboot`, a BMC power reset and `chassis power
# cycle` all leave Current unchanged — the BlueField-3 has its own power rail that survives them.
# See the README's "Firmware NV-config (PCC prerequisite)" section.
#
# Idempotent: writes nothing when the values are already live or already staged.
# admin/check_pcc_ready.sh is the read-only counterpart that reports where a machine stands.
#
# Usage:
#   ./set_pcc_nvconfig.sh          # stage both knobs
#   ./set_pcc_nvconfig.sh --emit   # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: set_pcc_nvconfig.sh [--emit]

Stage USER_PROGRAMMABLE_CC=1 and DPA_AUTHENTICATION=0 on this machine's BlueField. The values
go live only after a full power-cycle; a reboot does not apply them.

      --emit    print '@@key=value' lines (action, device, current, next) instead of the human
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

if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
else
	sudo -n true 2>/dev/null || die "passwordless sudo is required (logged in as $(id -un))"
	SUDO=(sudo -n)
fi

# PF0, by mst device if mst is started, otherwise by the PCI address behind the first IB device
# — mlxconfig takes either, and starting mst just to write a knob is not worth the side effect.
pick_device() {
	local dev ibdev
	for dev in /dev/mst/mt*_pciconf0; do
		[ -e "$dev" ] && { printf '%s\n' "$dev"; return 0; }
	done
	for ibdev in /sys/class/infiniband/*/device; do
		[ -e "$ibdev" ] || continue
		dev=$(basename "$(readlink -f "$ibdev")")
		case "$dev" in *.0) printf '%s\n' "$dev"; return 0 ;; esac
	done
	return 1
}

DEVICE=$(pick_device) || die "no BlueField device found (no /dev/mst/mt*_pciconf0, no mlx5 PCI device)"

# With -e the last three fields of a knob's line are Default, Current and Next Boot; "True(1)"
# and "False(0)" reduce to the number in parentheses.
read_knobs() {
	query=$("${SUDO[@]}" mlxconfig -d "$DEVICE" -e q 2>&1) \
		|| die "mlxconfig -d $DEVICE -e q failed: $(printf '%s' "$query" | tail -1)"

	local raw pair col
	for pair in "upcc:USER_PROGRAMMABLE_CC" "dpa:DPA_AUTHENTICATION"; do
		for col in current next; do
			raw=$(printf '%s\n' "$query" | awk -v knob="${pair#*:}" -v col="$col" '
				$0 ~ knob { print (col == "current") ? $(NF - 1) : $NF; exit }
			')
			[ -n "$raw" ] || die "could not parse ${pair#*:} out of mlxconfig on $DEVICE"
			if [[ "$raw" =~ \(([0-9]+)\) ]]; then
				raw="${BASH_REMATCH[1]}"
			fi
			printf -v "${pair%%:*}_$col" '%s' "$raw"
		done
	done
}

read_knobs

if [ "$upcc_current" = "1" ] && [ "$dpa_current" = "0" ]; then
	action="already-live"
	echo "already live on $DEVICE: USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0 — nothing to do"
elif [ "$upcc_next" = "1" ] && [ "$dpa_next" = "0" ]; then
	action="already-staged"
	echo "already staged on $DEVICE (current: $upcc_current/$dpa_current) — power-cycle to apply"
else
	echo "staging on $DEVICE: USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0"
	"${SUDO[@]}" mlxconfig -y -d "$DEVICE" set USER_PROGRAMMABLE_CC=1 DPA_AUTHENTICATION=0

	# Confirm from the device rather than trusting mlxconfig's exit code.
	read_knobs
	[ "$upcc_next" = "1" ] && [ "$dpa_next" = "0" ] \
		|| die "mlxconfig returned success but Next Boot is USER_PROGRAMMABLE_CC=$upcc_next DPA_AUTHENTICATION=$dpa_next"
	action="staged"
fi

if [ "$EMIT" -eq 1 ]; then
	emit action  "$action"
	emit device  "$DEVICE"
	emit current "$upcc_current/$dpa_current"
	emit next    "$upcc_next/$dpa_next"
	if [ "$action" = "already-live" ]; then
		emit subject "nothing to do"
	else
		emit subject "power-cycle required (a reboot will not apply it)"
	fi
	exit 0
fi

if [ "$action" != "already-live" ]; then
	cat <<EOF

Now POWER-CYCLE the DPU — a reboot is not enough. Power off, wait ~30s for the rail to
drain, power on, e.g. over IPMI to the host's BMC:

  ipmitool -H <bmc> -U <user> -P <pass> chassis power off
  sleep 30
  ipmitool -H <bmc> -U <user> -P <pass> chassis power on

Then check it took: ./check_pcc_ready.sh (Current must read 1 and 0).
EOF
fi
