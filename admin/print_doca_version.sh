#!/usr/bin/env bash
#
# Print the DOCA version installed on this machine, normalized to MAJOR.MINOR (e.g. "2.9").
#
# This is the single source of truth for "which DOCA is this box running", and the contract
# between a machine and the per-version source directory built for it: the string printed here
# is expected to name a directory in the repo. Normalizing to MAJOR.MINOR is deliberate — if a
# patch release ever needs its own directory, this script changes and nothing else does.
#
# Detection order (first hit wins), most to least authoritative:
#
#   1. pkg-config doca-common   — what meson actually links against, so it is the version that
#                                 decides whether a build succeeds. Absent on DOCA 2.7, which
#                                 ships no doca-common.pc at all (see the README's testbed B).
#   2. dpkg                     — the installed package version. Works on 2.7, and the package
#                                 naming changed across releases, so several names are tried.
#   3. VERSION file             — shipped with the sample applications under /opt/mellanox/doca.
#
# Usage:
#   ./print_doca_version.sh          # prints e.g. "2.9", or exits 1 if DOCA is not found
#   ./print_doca_version.sh -v       # also reports what every source said, on stderr
#   ./print_doca_version.sh --raw    # print the unnormalized string from the winning source
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: print_doca_version.sh [-v] [--raw]

Print the DOCA version installed on this machine, normalized to MAJOR.MINOR (e.g. "2.9").
Exits 1 if no DOCA installation is found.

  -v, --verbose   report what every detection source said, on stderr
      --raw       print the unnormalized string from the winning source
EOF
}

VERBOSE=0
RAW=0
for arg in "$@"; do
	case "$arg" in
		-v|--verbose) VERBOSE=1 ;;
		--raw)        RAW=1 ;;
		-h|--help)    usage; exit 0 ;;
		*)            echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

DOCA_ROOT="${DOCA_ROOT:-/opt/mellanox/doca}"

note() { [ "$VERBOSE" -eq 1 ] && echo "$*" >&2 || true; }

# MAJOR.MINOR out of anything: "2.9.1008" -> 2.9, "2.7.0085-1" -> 2.7, "doca 3.2.0" -> 3.2.
normalize() {
	printf '%s\n' "$1" | grep -oE '[0-9]+\.[0-9]+' | head -1
}

# --- source 1: pkg-config ----------------------------------------------------------------------
# DOCA installs its .pc files under the arch-specific libdir, which is not on the default
# pkg-config search path on every image, so add the candidates ourselves rather than relying on
# the environment being set up.
from_pkgconfig() {
	command -v pkg-config >/dev/null 2>&1 || { note "pkg-config: not installed"; return 1; }

	local extra
	extra=$(ls -d "$DOCA_ROOT"/lib/*/pkgconfig 2>/dev/null | paste -sd: -) || true
	[ -n "$extra" ] && export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$extra"

	local v
	if v=$(pkg-config --modversion doca-common 2>/dev/null) && [ -n "$v" ]; then
		note "pkg-config doca-common: $v"
		printf '%s\n' "$v"
		return 0
	fi
	note "pkg-config doca-common: not found (expected on DOCA 2.7)"
	return 1
}

# --- source 2: dpkg ----------------------------------------------------------------------------
# Package naming moved around across releases: 2.7 ships doca-devel / doca-runtime, later
# releases split into doca-sdk-* and doca-all. Try the specific names first, then fall back to
# whatever doca package is installed and carries a version.
from_dpkg() {
	command -v dpkg-query >/dev/null 2>&1 || { note "dpkg-query: not installed"; return 1; }

	local pkg v
	for pkg in doca-sdk-common doca-common doca-all doca-devel doca-runtime doca-sdk; do
		if v=$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null) && [ -n "$v" ]; then
			note "dpkg $pkg: $v"
			printf '%s\n' "$v"
			return 0
		fi
	done

	# Last resort: first installed package whose name starts with doca-.
	v=$(dpkg-query -W -f='${Package} ${Version}\n' 'doca-*' 2>/dev/null \
		| awk 'NF == 2 { print $2; exit }') || true
	if [ -n "${v:-}" ]; then
		note "dpkg doca-*: $v"
		printf '%s\n' "$v"
		return 0
	fi

	note "dpkg: no doca packages installed"
	return 1
}

# --- source 3: the VERSION file shipped with the sample applications ---------------------------
from_version_file() {
	local f v
	for f in "$DOCA_ROOT/applications/VERSION" "$DOCA_ROOT/VERSION"; do
		if [ -r "$f" ] && v=$(tr -d '[:space:]' < "$f") && [ -n "$v" ]; then
			note "$f: $v"
			printf '%s\n' "$v"
			return 0
		fi
	done
	note "VERSION file: not found under $DOCA_ROOT"
	return 1
}

# --- resolve -----------------------------------------------------------------------------------
# First hit wins. In verbose mode every source is probed anyway, so that -v reports what the
# whole box looks like rather than stopping at the first answer — useful when the sources
# disagree, which is the interesting case.
raw=""
winner=""
for source in from_pkgconfig from_dpkg from_version_file; do
	[ -n "$raw" ] && [ "$VERBOSE" -eq 0 ] && break
	if v=$($source) && [ -z "$raw" ]; then
		raw="$v"
		winner="$source"
	fi
done
[ -n "$winner" ] && note "using: $winner"

if [ -z "$raw" ]; then
	echo "ERROR: no DOCA installation found (looked at pkg-config, dpkg and $DOCA_ROOT)" >&2
	exit 1
fi

if [ "$RAW" -eq 1 ]; then
	printf '%s\n' "$raw"
	exit 0
fi

version=$(normalize "$raw")
if [ -z "$version" ]; then
	echo "ERROR: could not parse a MAJOR.MINOR version out of '$raw'" >&2
	exit 1
fi

printf '%s\n' "$version"
