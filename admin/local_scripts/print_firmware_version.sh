#!/usr/bin/env bash
#
# Print the ConnectX/BlueField firmware version running on this machine (e.g. "32.43.2402").
#
# This is the NIC firmware — the number `mlxlink` calls "Firmware Version" and `mlxfwmanager`
# calls FW — not the BSP/bf-bundle version that `bfver` reports. It is the one that matters here:
# the DPA and PCC exercises depend on firmware NV-config knobs and on DPA behaviour that differs
# between firmware builds (see the README's firmware NV-config section), so a fleet on mixed
# firmware will not behave uniformly under the same instructions.
#
# Detection order (first hit wins), most to least convenient:
#
#   1. sysfs        — /sys/class/infiniband/<dev>/fw_ver, what the loaded mlx5 driver reports.
#                     Needs no root and no MFT, so it is the one that works on a stock Arm.
#   2. ethtool -i   — same number by way of the netdev, for a box whose IB devices are absent.
#   3. mlxfwmanager — the MFT query. Authoritative, but wants mst started and usually root.
#
# Usage:
#   ./print_firmware_version.sh          # prints e.g. "32.43.2402", or exits 1 if not found
#   ./print_firmware_version.sh -v       # also reports what every source said, on stderr
#   ./print_firmware_version.sh --emit   # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: print_firmware_version.sh [-v] [--emit]

Print the ConnectX/BlueField firmware version running on this machine (e.g. "32.43.2402").
Exits 1 if no firmware version can be determined.

  -v, --verbose   report what every detection source said, on stderr
      --emit      print '@@key=value' lines (fw, fw_device, fw_source) instead of the bare
                  version, which is what admin/fleet.py parses
EOF
}

VERBOSE=0
EMIT=0
for arg in "$@"; do
	case "$arg" in
		-v|--verbose) VERBOSE=1 ;;
		--emit)       EMIT=1 ;;
		-h|--help)    usage; exit 0 ;;
		*)            echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

note() { [ "$VERBOSE" -eq 1 ] && echo "$*" >&2 || true; }

# Every source prints "<version> <device>" on one line: the sources run in a command
# substitution, so they cannot hand the device name back through a variable.

# --- source 1: sysfs ---------------------------------------------------------------------------
# A BlueField in DPU mode exposes one IB device per port (mlx5_0, mlx5_1); both are the same
# silicon and report the same firmware, so the first one answers the question.
from_sysfs() {
	local f v dev
	for f in /sys/class/infiniband/*/fw_ver; do
		[ -r "$f" ] || continue
		v=$(tr -d '[:space:]' < "$f") || continue
		[ -n "$v" ] || continue
		dev=$(basename "$(dirname "$f")")
		note "sysfs $f: $v"
		printf '%s %s\n' "$v" "$dev"
		return 0
	done
	note "sysfs: no readable /sys/class/infiniband/*/fw_ver"
	return 1
}

# --- source 2: ethtool -------------------------------------------------------------------------
# 'firmware-version: 32.43.2402 (MT_0000000884)' — the parenthesised part is the board PSID, not
# the version, so keep the first field only.
from_ethtool() {
	command -v ethtool >/dev/null 2>&1 || { note "ethtool: not installed"; return 1; }

	local iface driver v
	for iface in /sys/class/net/*; do
		[ -e "$iface/device" ] || continue  # skip lo and other virtual interfaces
		driver=$(basename "$(readlink -f "$iface/device/driver" 2>/dev/null || echo none)")
		[ "$driver" = "mlx5_core" ] || continue

		iface=$(basename "$iface")
		v=$(ethtool -i "$iface" 2>/dev/null | awk '/^firmware-version:/ { print $2; exit }') || true
		if [ -n "${v:-}" ]; then
			note "ethtool -i $iface: $v"
			printf '%s %s\n' "$v" "$iface"
			return 0
		fi
	done
	note "ethtool: no mlx5_core interface reported a firmware version"
	return 1
}

# --- source 3: mlxfwmanager --------------------------------------------------------------------
# Output is a per-device block with an 'FW <current> <available>' row; the first column is what is
# running. Needs the mst devices present, and normally root — hence last.
from_mlxfwmanager() {
	command -v mlxfwmanager >/dev/null 2>&1 || { note "mlxfwmanager: not installed"; return 1; }

	local out v dev
	out=$(mlxfwmanager --query 2>/dev/null) || { note "mlxfwmanager: query failed (needs root?)"; return 1; }

	v=$(printf '%s\n' "$out" | awk '$1 == "FW" { print $2; exit }') || true
	if [ -n "${v:-}" ]; then
		dev=$(printf '%s\n' "$out" | awk '/^Device Type:/ { print $3; exit }') || true
		note "mlxfwmanager: $v"
		printf '%s %s\n' "$v" "${dev:-mlxfwmanager}"
		return 0
	fi
	note "mlxfwmanager: no FW row in the query output"
	return 1
}

# --- resolve -----------------------------------------------------------------------------------
# First hit wins. In verbose mode every source is probed anyway, so that -v shows whether the
# sources agree — the interesting case is when they do not.
version=""
winner=""
winner_device=""
for source in from_sysfs from_ethtool from_mlxfwmanager; do
	[ -n "$version" ] && [ "$VERBOSE" -eq 0 ] && break
	if answer=$($source) && [ -z "$version" ]; then
		version="${answer%% *}"
		winner_device="${answer#* }"
		winner="$source"
	fi
done
[ -n "$winner" ] && note "using: $winner"

if [ -z "$version" ]; then
	echo "ERROR: no firmware version found (looked at sysfs, ethtool and mlxfwmanager)" >&2
	exit 1
fi

if [ "$EMIT" -eq 1 ]; then
	# The function name is an implementation detail; report the source the way a human names it.
	case "$winner" in
		from_sysfs)        source_name="sysfs" ;;
		from_ethtool)      source_name="ethtool" ;;
		from_mlxfwmanager) source_name="mlxfwmanager" ;;
		*)                 source_name="$winner" ;;
	esac
	printf '@@fw=%s\n'        "$version"
	printf '@@fw_device=%s\n' "${winner_device:-?}"
	printf '@@fw_source=%s\n' "$source_name"
	exit 0
fi

printf '%s\n' "$version"
