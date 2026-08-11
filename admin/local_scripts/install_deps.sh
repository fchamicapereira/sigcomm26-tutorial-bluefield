#!/usr/bin/env bash
#
# Target-side: install everything the tutorial needs to build and run NATIVELY on the Arm.
#
# The package list is the Dockerfiles' apt blocks (doca-*/Dockerfile) plus what the repo's own
# scripts reach for — setup_ttyplot.sh, benchmark.sh, run_{server,client}.sh, roce_loopback.py.
# The DOCA devel image starts from a base that already carries the build toolchain; a stock BSP
# install does not, so the toolchain is spelled out here even though no Dockerfile mentions it.
#
# fleet.py pipes this over stdin (`ssh <host> bash -s -- <args>`), so it MUST stay self-contained.
# It is also meant to be runnable by hand on any machine the fleet tool cannot reach.
#
# What this deliberately does NOT do:
#
#   * touch anything DOCA ships. DOCA, dpacc, MFT (mlxconfig/mlxfwreset), pyverbs and — on a BSP
#     image — perftest come from NVIDIA's install, and the distro's versions of some of those
#     will happily conflict with it. Those are verified and reported, never installed. perftest
#     is the one exception, installed only when ib_write_bw is missing entirely.
#   * build ttyplot. That is setup_ttyplot.sh's job (it clones into the repo); this only puts its
#     build dependencies in place so that script works.
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
#
# Reporting is the default and installing is opt-in: this runs against a fleet of shared lab
# machines, where finding out what is missing should never be the same keystroke as changing them.
#
# Usage:
#   ./install_deps.sh            # report what is missing, change nothing
#   ./install_deps.sh --install  # install what is missing
#   ./install_deps.sh --emit     # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: install_deps.sh [--install] [--emit]

Report — or with --install, install — the packages needed to build and run the tutorial
natively on the Arm. Without --install nothing on the machine is touched.

      --install   install the missing packages
      --emit      print '@@key=value' lines (added, needed, present, tools) instead of the
                  human summary, which is what admin/fleet.py parses
EOF
}

INSTALL=0
EMIT=0
for arg in "$@"; do
	case "$arg" in
		--install) INSTALL=1 ;;
		--emit)    EMIT=1 ;;
		-h|--help) usage; exit 0 ;;
		*)         echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

# --- what we install ---------------------------------------------------------------------------
# Grouped by why, because a bare list ages badly: when one of these turns out to be unnecessary
# the next person needs to know what it was for before dropping it.
PACKAGES=(
	# Build toolchain. The README's requirements section: meson + ninja + dpacc are all that is
	# needed to build both parts on the Arm, and dpacc is DOCA's.
	build-essential
	meson
	ninja-build
	pkg-config
	git

	# Part IV copies /opt/mellanox/doca/applications/pcc and patches it (pureecn_dcqcn.patch).
	patch

	# meson.build picks up libbsd optionally (-D DOCA_USE_LIBBSD); doca_flow_mirror and
	# doca_flow_ecn_pcap write captured packets to a .pcap and link -lpcap.
	libbsd-dev
	libpcap-dev

	# check_ecn_bits_from_pcap.sh decodes the capture with tcpdump to show the ECN codepoints.
	tcpdump

	# dpacc/DPDK use pyelftools when building the DPA algo.
	python3-pyelftools

	# setup_ttyplot.sh builds ttyplot from source for benchmark.sh's live throughput chart.
	libncurses-dev

	# Driving the exercises: tmux for server+client side by side, ethtool for link state,
	# sudo because every tutorial script calls it for its privileged steps.
	tmux
	ethtool
	sudo
)

# Installed only if the binary is absent: a BSP/DOCA image already ships perftest, and the
# distro package would replace a build that is matched to the installed rdma-core.
declare -A CONDITIONAL=(
	[ib_write_bw]=perftest
)

# Must come from the DOCA/BSP install. Reported when missing so the machine can be fixed by
# hand; never apt-installed, because the distro versions conflict with what NVIDIA ships.
NOT_OURS=(
	"/opt/mellanox/doca:DOCA"
	"/opt/mellanox/doca/tools/dpacc:dpacc"
	"/opt/mellanox/dpdk/bin/dpdk-hugepages.py:dpdk-hugepages"
	"cmd:mlxconfig:MFT"
	"cmd:mlxfwreset:MFT"
	"py:pyverbs:python3-pyverbs"
)

emit() { printf '@@%s=%s\n' "$1" "$2"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

command -v dpkg-query >/dev/null 2>&1 || die "this is not a dpkg-based system"

# Only installing needs root — reporting must work on a machine where this account cannot sudo,
# since that is itself worth finding out about rather than failing on.
SUDO=()
if [ "$INSTALL" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
	sudo -n true 2>/dev/null || die "passwordless sudo is required to install (logged in as $(id -un))"
	SUDO=(sudo -n)
fi

# --- what is already here ----------------------------------------------------------------------
installed_ok() {
	# 'install ok installed' is the only status that means the files are actually on disk:
	# a removed-but-not-purged package still has a dpkg entry.
	[ "$(dpkg-query -W -f='${db:Status-Status}' "$1" 2>/dev/null || true)" = "installed" ]
}

wanted=()
for pkg in "${PACKAGES[@]}"; do
	installed_ok "$pkg" || wanted+=("$pkg")
done

for bin in "${!CONDITIONAL[@]}"; do
	pkg="${CONDITIONAL[$bin]}"
	if command -v "$bin" >/dev/null 2>&1; then
		echo "$bin already present, not installing $pkg"
	elif ! installed_ok "$pkg"; then
		wanted+=("$pkg")
	fi
done

present=$(( ${#PACKAGES[@]} - ${#wanted[@]} ))

# --- what DOCA owes us -------------------------------------------------------------------------
missing_tools=()
for entry in "${NOT_OURS[@]}"; do
	case "$entry" in
		cmd:*)
			name="${entry#cmd:}"; label="${name#*:}"; name="${name%%:*}"
			command -v "$name" >/dev/null 2>&1 || missing_tools+=("$label")
			;;
		py:*)
			name="${entry#py:}"; label="${name#*:}"; name="${name%%:*}"
			python3 -c "import $name" >/dev/null 2>&1 || missing_tools+=("$label")
			;;
		*)
			path="${entry%%:*}"; label="${entry#*:}"
			[ -e "$path" ] || missing_tools+=("$label")
			;;
	esac
done
# The same label can be owed by several entries (mlxconfig and mlxfwreset are both MFT).
if [ ${#missing_tools[@]} -gt 0 ]; then
	mapfile -t missing_tools < <(printf '%s\n' "${missing_tools[@]}" | sort -u)
fi

# --- install -----------------------------------------------------------------------------------
added=()
if [ ${#wanted[@]} -eq 0 ]; then
	echo "nothing to install: all ${#PACKAGES[@]} packages already present"
elif [ "$INSTALL" -eq 0 ]; then
	echo "missing (re-run with --install): ${wanted[*]}"
else
	echo "installing: ${wanted[*]}"
	export DEBIAN_FRONTEND=noninteractive
	# Lock::Timeout rather than failing outright: on a freshly booted machine unattended-upgrades
	# is usually still holding the dpkg lock, and a fleet-wide run must not lose a machine to it.
	APT_OPTS=(-y -o DPkg::Lock::Timeout=300)
	"${SUDO[@]}" apt-get "${APT_OPTS[@]}" update
	"${SUDO[@]}" apt-get "${APT_OPTS[@]}" install --no-install-recommends "${wanted[@]}"

	# Report what actually landed rather than what was asked for, so a package that silently
	# failed to install is not counted as a success.
	failed=()
	for pkg in "${wanted[@]}"; do
		if installed_ok "$pkg"; then added+=("$pkg"); else failed+=("$pkg"); fi
	done
	if [ ${#failed[@]} -gt 0 ]; then
		die "apt reported success but these are still not installed: ${failed[*]}"
	fi
fi

join() { local IFS=,; if [ $# -eq 0 ]; then echo "-"; else echo "$*"; fi; }

if [ "$EMIT" -eq 1 ]; then
	emit added   "$(join "${added[@]}")"
	emit needed  "$(join "${wanted[@]}")"
	emit present "$present/${#PACKAGES[@]}"
	emit tools   "$(join "${missing_tools[@]}")"
	exit 0
fi

echo
echo "packages already present: $present/${#PACKAGES[@]}"
if [ ${#added[@]} -gt 0 ]; then
	echo "installed: ${added[*]}"
fi
if [ ${#missing_tools[@]} -gt 0 ]; then
	echo "MISSING (not installable from here — comes with DOCA/BSP): ${missing_tools[*]}"
fi
