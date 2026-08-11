#!/usr/bin/env bash
#
# Target-side: run the whole tutorial end to end on this machine and say whether it worked.
#
# The other local_scripts report what a machine *is* — which DOCA, which firmware, which packages.
# This one reports whether it *works*: it builds the version this box needs, drives RoCE traffic
# across the two ports, checks that DOCA Flow marks ECN and captures to a pcap, then loads the DOCA
# PCC controller and checks the rate actually collapses. Ten phases, all or nothing.
#
# It is DESTRUCTIVE. Phase 1 runs setup_roce_loopback.sh, which deletes every OVS bridge and every
# SF on both PFs, and the traffic phases will fight with anything already using the ports. Do not
# point it at a machine somebody is working on. Everything before phase 1 is read-only, though, so
# a machine this test cannot serve (wrong DOCA version) is left untouched.
#
# Unlike sync.sh this may assume the repo is already checked out — it is testing that checkout — so
# it calls the repo's own scripts (setup_roce_loopback.sh, print_doca_version.sh, check_pcc_ready.sh,
# run_server.sh, run_client.sh) instead of reimplementing them.
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
# Per-process logs are left behind in $LOGDIR for whatever the summary does not explain.
#
# Usage:
#   ./test_tutorial.sh                      # full run, defaults
#   ./test_tutorial.sh --skip-build         # reuse the existing build/ (much faster to iterate)
#   ./test_tutorial.sh --skip-pcc           # DOCA Flow half only
#   ./test_tutorial.sh --emit               # print '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

# --- defaults -----------------------------------------------------------------------------------
REPO="${TUTORIAL_REPO:-/home/s26t/sigcomm26-tutorial-bluefield}"
LOGDIR="${TUTORIAL_TEST_LOGDIR:-}"   # empty => a fresh mktemp -d under /tmp, see below
PCAP=""                              # empty => capture.pcap inside $LOGDIR
SAMPLE=10000        # --sample: write ~1-in-N of the captured packets
PERCENT=100         # --percent: CE-mark this share of wire IPv4 (100 => every packet)
PCC_DEV=""          # empty => derive from PF1's PCI address
WARMUP=10           # seconds of traffic before anything is measured
WINDOW=10           # seconds averaged for each throughput number
CAPTURE=10          # seconds with pcap writing enabled
SETTLE=15           # seconds after loading PCC before the second throughput window
MIN_GBPS=10         # the baseline has to clear this, or the "traffic is flowing" claim is hollow
MAX_RATIO=0.5       # PCC passes when bw_pcc <= MAX_RATIO * bw_base
SKIP_BUILD=0
SKIP_PCC=0
FORCE=0
EMIT=0

# setup_roce_loopback.sh imposes this layout; PF1 is the reaction point the PCC controller attaches
# to. Same env var name, so overriding it once covers both scripts.
PF1_PCI="${PF1_PCI:-0000:03:00.1}"

usage() {
	cat <<'EOF'
Usage: test_tutorial.sh [options]

Run the SIGCOMM'26 BlueField-3 tutorial end to end on this machine and report pass/fail.

DESTRUCTIVE: deletes every OVS bridge and SF on both PFs (via setup_roce_loopback.sh) and drives
the ports at line rate. Everything before that is read-only.

Options:
  --repo DIR        tutorial checkout (default: /home/s26t/sigcomm26-tutorial-bluefield)
  --pcap PATH       capture file to write and inspect (default: capture.pcap in the run's log dir)
  --sample N        doca_flow_ecn_pcap --sample (default: 10000)
  --percent P       doca_flow_ecn_pcap --percent (default: 100)
  --pcc-dev NAME    IB device for the PCC controller (default: derived from PF1's PCI address)
  --warmup SEC      traffic settling time before measuring (default: 10)
  --window SEC      seconds averaged per throughput number (default: 10)
  --capture SEC     seconds with pcap writing enabled (default: 10)
  --settle SEC      seconds after loading PCC before re-measuring (default: 15)
  --min-gbps X      baseline throughput floor (default: 10)
  --max-ratio R     PCC passes when bw_pcc <= R * bw_base (default: 0.5)
  --skip-build      reuse the existing build directory instead of rebuilding from scratch
  --skip-pcc        run the DOCA Flow half only
  --force           run even when another DOCA/DPDK workload is already using this machine
  --emit            print '@@key=value' lines for admin/fleet.py
  -h, --help        show this help and exit
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--repo)       REPO="$2";       shift 2 ;;
		--pcap)       PCAP="$2";       shift 2 ;;
		--sample)     SAMPLE="$2";     shift 2 ;;
		--percent)    PERCENT="$2";    shift 2 ;;
		--pcc-dev)    PCC_DEV="$2";    shift 2 ;;
		--warmup)     WARMUP="$2";     shift 2 ;;
		--window)     WINDOW="$2";     shift 2 ;;
		--capture)    CAPTURE="$2";    shift 2 ;;
		--settle)     SETTLE="$2";     shift 2 ;;
		--min-gbps)   MIN_GBPS="$2";   shift 2 ;;
		--max-ratio)  MAX_RATIO="$2";  shift 2 ;;
		--skip-build) SKIP_BUILD=1;    shift ;;
		--skip-pcc)   SKIP_PCC=1;      shift ;;
		--force)      FORCE=1;         shift ;;
		--emit)       EMIT=1;          shift ;;
		-h|--help)    usage; exit 0 ;;
		*) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

# --- output -------------------------------------------------------------------------------------
log()  { printf '%s\n' "$*"; }
emit() { [ "$EMIT" -eq 1 ] && printf '@@%s=%s\n' "$1" "$2" || true; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# Dump the tail of a process log into our own output, so the fleet-level log carries the evidence
# and nobody has to ssh in to find out what broke.
show_tail() {
	local file=$1 lines=${2:-25}
	[ -s "$file" ] || { log "   (${file}: empty)"; return 0; }
	log "   --- last ${lines} lines of ${file}:"
	tail -n "$lines" "$file" | sed 's/^/   | /'
}

# --- phase bookkeeping --------------------------------------------------------------------------
# Every phase is a function that returns 0/1 and leaves a one-line explanation in STEP_DETAIL.
#
# NOTE for anyone editing a phase: `set -e` does NOT apply inside these functions, because bash
# disables errexit for any function invoked in a condition context and `step` invokes them with
# `if`. So every command that matters must be checked explicitly:
#     cmd ... || { STEP_DETAIL="what went wrong"; return 1; }
STEP_NAMES=()
STEP_STATES=()
STEP_DETAILS=()
STEP_DETAIL=""

step() { # step <name> <function> [args...]
	local name=$1
	shift
	STEP_DETAIL=""
	log ""
	log "== ${name} =="
	if "$@"; then
		STEP_NAMES+=("$name"); STEP_STATES+=("ok"); STEP_DETAILS+=("$STEP_DETAIL")
		log "-- ok${STEP_DETAIL:+: $STEP_DETAIL}"
		return 0
	fi
	STEP_NAMES+=("$name"); STEP_STATES+=("FAIL"); STEP_DETAILS+=("$STEP_DETAIL")
	log "-- FAIL${STEP_DETAIL:+: $STEP_DETAIL}"
	return 1
}

# --- process helpers ----------------------------------------------------------------------------
# The pid of a `sudo -n <binary>` job is sudo's, not the binary's, and whether sudo execs in place
# or forks and waits varies with how it was built. Find the process by its command line and skip
# the sudo wrapper if one is there. /proc is world-readable, so this needs no privileges.
real_pid() { # real_pid <absolute binary path>
	local p
	for p in $(pgrep -f "$1" 2>/dev/null || true); do
		[ "$(cat "/proc/$p/comm" 2>/dev/null || true)" = "sudo" ] && continue
		printf '%s\n' "$p"
		return 0
	done
	return 1
}

wait_for_line() { # wait_for_line <log> <extended regex> <timeout seconds>
	local log=$1 pattern=$2 timeout=$3 waited=0
	while [ "$waited" -lt "$timeout" ]; do
		grep -Eq -- "$pattern" "$log" 2>/dev/null && return 0
		sleep 1
		waited=$((waited + 1))
	done
	return 1
}

# Mean of the last N throughput samples in an ib_write_bw log. With --report_gbits
# --run_infinitely it prints one row per reporting period:
#
#   #bytes  #iterations  BW peak[Gb/sec]  BW average[Gb/sec]  MsgRate[Mpps]
#    65536      1234          0.00             92.60             0.176643
#
# Those rows are the only lines that are exactly five bare numbers, which is what the guard
# matches — the surrounding banner and the column headers are not. Field 4 is the average.
# Prints nothing and returns 1 if the log has no rows yet.
bw_mean() { # bw_mean <log> <n>
	awk -v n="$2" '
		$0 ~ /^[[:space:]]*[0-9]+([[:space:]]+[0-9.]+){4}[[:space:]]*$/ { v[++c] = $4 }
		END {
			if (c == 0) exit 1
			start = c - n + 1
			if (start < 1) start = 1
			for (i = start; i <= c; i++) total += v[i]
			printf "%.2f\n", total / (c - start + 1)
		}' "$1"
}

# Throughput as reported by the server, falling back to the client. ib_write_bw prints the table on
# both ends in --run_infinitely mode, but the request asks for the server's number and the server is
# the side actually receiving.
traffic_mean() { # traffic_mean <n>
	bw_mean "$SERVER_LOG" "$1" 2>/dev/null || bw_mean "$CLIENT_LOG" "$1" 2>/dev/null || return 1
}

# --- cleanup ------------------------------------------------------------------------------------
# Runs on every exit path, including a failed phase, an unexpected error and Ctrl-C. Leaving an
# ib_write_bw pair or a DPA program loaded behind would poison the next run of this test, and the
# next person's manual session too.
CLEANUP_DONE=0
cleanup() {
	[ "$CLEANUP_DONE" -eq 1 ] && return 0
	CLEANUP_DONE=1
	log ""
	log "== cleanup =="

	# Every pattern below is guarded against being empty: this runs as root, and `pkill -f ""`
	# matches every process on the machine. The trap is installed only after these are set, so an
	# empty one should be impossible — which is exactly the kind of assumption worth not betting a
	# DPU on.
	# Traffic first: killing the flow program out from under a running ib_write_bw pair just makes
	# them error out noisily.
	sudo -n pkill -f 'ib_write_bw' 2>/dev/null || true
	for launcher in "$CLIENT_LAUNCHER" "$SERVER_LAUNCHER"; do
		[ -n "$launcher" ] && kill "$launcher" 2>/dev/null
	done || true

	[ -n "$PCC_BIN" ] && sudo -n pkill -f "$PCC_BIN" 2>/dev/null || true
	[ -n "$FLOW_BIN" ] || { log "   logs left in ${LOGDIR}"; return 0; }

	# SIGINT, not SIGKILL: the flow program closes the pcap on its way out, and a half-written
	# final record is exactly the thing that makes the capture unreadable afterwards.
	local pid
	if pid=$(real_pid "$FLOW_BIN"); then
		sudo -n kill -INT "$pid" 2>/dev/null || true
		local waited=0
		while [ "$waited" -lt 10 ] && sudo -n kill -0 "$pid" 2>/dev/null; do
			sleep 1
			waited=$((waited + 1))
		done
	fi
	sudo -n pkill -f "$FLOW_BIN" 2>/dev/null || true

	log "   logs left in ${LOGDIR}"
}

# --- preflight (nothing here touches the machine's configuration) --------------------------------
CLIENT_LAUNCHER=""
SERVER_LAUNCHER=""
FLOW_BIN=""
PCC_BIN=""

# A fresh directory per run rather than a fixed /tmp/test_tutorial. These are shared lab machines
# with more than one account on them, and a fixed path is a path somebody else's run may already own:
# bf3-uwaterloo-1 had a ubuntu-owned /tmp/test_tutorial, and `mkdir -p` on a directory owned by
# another user *succeeds*, so every phase then failed on its log redirect ("Permission denied") with
# nothing wrong with the machine. mktemp cannot collide, and an explicit TUTORIAL_TEST_LOGDIR still
# gets checked for writability so that case fails here, once, with a straight answer.
if [ -n "$LOGDIR" ]; then
	mkdir -p "$LOGDIR" 2>/dev/null || true
	[ -d "$LOGDIR" ] && [ -w "$LOGDIR" ] ||
		die "TUTORIAL_TEST_LOGDIR=${LOGDIR} is not a directory $(id -un) can write to"
else
	LOGDIR=$(mktemp -d /tmp/test_tutorial.XXXXXXXX) || die "could not create a log directory under /tmp"
fi
PCAP="${PCAP:-$LOGDIR/capture.pcap}"

SERVER_LOG="$LOGDIR/server.log"
CLIENT_LOG="$LOGDIR/client.log"
FLOW_LOG="$LOGDIR/doca_flow_ecn_pcap.log"
PCC_LOG="$LOGDIR/doca_pcc_ecn_rp.log"
BUILD_LOG="$LOGDIR/build.log"
SETUP_LOG="$LOGDIR/setup_roce_loopback.log"

log "== preflight =="
log "   repo:   ${REPO}"
log "   logs:   ${LOGDIR}"

[ -d "$REPO" ] || die "tutorial checkout not found at ${REPO} (run the 'sync' verb first, or pass --repo)"
SCRIPTS="$REPO/admin/local_scripts"
[ -r "$SCRIPTS/print_doca_version.sh" ] || die "${SCRIPTS}/print_doca_version.sh is missing — is ${REPO} up to date?"

# Which doca-*/ directory serves this machine, answered FIRST — before the checks on the rest of
# the checkout and the build tooling. A machine no directory targets is not going to be touched no
# matter what else is wrong with it, and "DOCA 3.1, unsupported" is a far more useful answer than
# "your checkout is stale" or "meson is missing" on a box that could not run the tutorial anyway.
#
# The mapping is not mechanical: doca-2/ covers every DOCA 2.x via the compile-time switches in
# doca_flow_compat.h, while the 3.x minors each get their own directory because the Flow API
# changed under them. A release with no directory is a real answer — the fleet has machines on
# DOCA 3.1, and nothing here targets 3.1.
DOCA_VERSION=$("$SCRIPTS/print_doca_version.sh" 2>/dev/null) || die "could not determine the DOCA version (see print_doca_version.sh -v)"
case "$DOCA_VERSION" in
	2.*) VERSION_DIR=doca-2 ;;
	3.2) VERSION_DIR=doca-3.2 ;;
	3.4) VERSION_DIR=doca-3.4 ;;
	*)   VERSION_DIR="" ;;
esac
emit doca "$DOCA_VERSION"

unsupported() {
	log ""
	log "== unsupported =="
	log "   $*"
	log "   nothing was changed on this machine."
	emit version unsupported
	emit verdict unsupported
	emit subject "$*"
	exit 1
}

[ -n "$VERSION_DIR" ] || unsupported "DOCA ${DOCA_VERSION}: no doca-*/ directory targets this release"
[ -d "$REPO/$VERSION_DIR" ] || unsupported "DOCA ${DOCA_VERSION} maps to ${VERSION_DIR}/, which is not in this checkout"

# Check for the sources rather than hardcoding which directory is incomplete: doca-3.2/ has no
# ecn_pcap today and should start passing the moment one lands there, with no edit here.
[ -r "$REPO/$VERSION_DIR/doca-flow/doca_flow_ecn_pcap.c" ] ||
	unsupported "${VERSION_DIR}/ has no doca-flow/doca_flow_ecn_pcap.c — the DOCA Flow exercise is not ported to DOCA ${DOCA_VERSION} yet"
if [ "$SKIP_PCC" -eq 0 ]; then
	[ -r "$REPO/$VERSION_DIR/doca-pcc-ecn/meson.build" ] ||
		unsupported "${VERSION_DIR}/ has no doca-pcc-ecn/ — the DOCA PCC exercise is not ported to DOCA ${DOCA_VERSION} yet (or pass --skip-pcc)"
fi

# Only now, once this machine is one we can actually serve, insist on the rest.
for f in check_pcc_ready.sh setup_roce_loopback.sh pcap_ecn_stats.py; do
	[ -r "$SCRIPTS/$f" ] || die "${SCRIPTS}/${f} is missing — is ${REPO} up to date?"
done
for f in run_server.sh run_client.sh; do
	[ -r "$REPO/$f" ] || die "${REPO}/${f} is missing — is ${REPO} up to date?"
done

sudo -n true 2>/dev/null || die "passwordless sudo is required (logged in as $(id -un))"

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "$1 is not on PATH${2:+ — $2}"; }
need_cmd meson "install it with the 'deps --install' verb"
need_cmd ninja "install ninja-build with the 'deps --install' verb"
need_cmd python3
need_cmd pgrep
need_cmd stdbuf "run_server.sh/run_client.sh need it to stream their output"
need_cmd ib_write_bw "comes from the perftest package"
if [ "$SKIP_PCC" -eq 0 ]; then
	[ -x /opt/mellanox/doca/tools/dpacc ] || die "/opt/mellanox/doca/tools/dpacc is missing — the PCC device code cannot be built (or pass --skip-pcc)"
fi

BUILD_DIR="$REPO/$VERSION_DIR/build"
FLOW_BIN="$BUILD_DIR/doca-flow/doca_flow_ecn_pcap"
PCC_BIN="$BUILD_DIR/doca-pcc-ecn/doca_pcc_ecn_rp"

log "   doca:   ${DOCA_VERSION} -> ${VERSION_DIR}/"
emit version "$VERSION_DIR"

# The PCC controller attaches to PF1, the reaction point. Ask the kernel which IB device that PCI
# function owns rather than trusting mlx5_1 to be it.
if [ -z "$PCC_DEV" ]; then
	PCC_DEV=$(ls "/sys/bus/pci/devices/$PF1_PCI/infiniband" 2>/dev/null | head -1 || true)
	PCC_DEV="${PCC_DEV:-mlx5_1}"
fi

# Firmware knobs. Not fatal — the DOCA Flow half is worth testing on a machine whose PCC knobs were
# never set — but the PCC half cannot run, and saying so beats a confusing failure ten minutes in.
PCC_SKIP_REASON=""
if [ "$SKIP_PCC" -eq 1 ]; then
	PCC_SKIP_REASON="--skip-pcc"
else
	pcc_verdict=$("$SCRIPTS/check_pcc_ready.sh" --emit 2>/dev/null | sed -n 's/^@@verdict=//p' | head -1 || true)
	case "${pcc_verdict:-unknown}" in
		ready) log "   pcc:    ${PCC_DEV} (firmware knobs live)" ;;
		*)     PCC_SKIP_REASON="firmware knobs not live (pcc-ready: ${pcc_verdict:-unknown})" ;;
	esac
fi
[ -n "$PCC_SKIP_REASON" ] && log "   pcc:    skipped — ${PCC_SKIP_REASON}"

# --- is anybody else using this machine? --------------------------------------------------------
# The first phase deletes every OVS bridge and SF on both PFs. That is harmless on an idle DPU and
# hostile on one somebody is working on, and these are shared lab machines — bf3-uwaterloo-1 had
# another user's doca_pcc and doca_flow_steer running from /home/ubuntu when this check was
# written, and wiping the SFs out from under them was not ours to do.
#
# Nothing of ours is running yet at this point, so *any* DPDK/DOCA/perftest process here is either
# somebody else's or the wreckage of an earlier crashed run. Both are reasons to stop and look.
BUSY=$(pgrep -af 'doca_|ib_(write|send|read)_bw|testpmd' 2>/dev/null | grep -v -- ' sudo ' || true)
if [ -n "$BUSY" ]; then
	if [ "$FORCE" -eq 0 ]; then
		log ""
		log "== in use =="
		log "   another DOCA/DPDK workload is running here:"
		printf '%s\n' "$BUSY" | sed 's/^/     /'
		log ""
		log "   refusing to wipe this machine's OVS bridges and SFs out from under it."
		log "   nothing was changed. Pass --force if that workload is yours and expendable."
		emit verdict busy
		emit subject "in use by another workload ($(printf '%s\n' "$BUSY" | wc -l | tr -d ' ') process(es)) — not touched"
		exit 1
	fi
	log "   busy: --force given, ignoring $(printf '%s\n' "$BUSY" | wc -l | tr -d ' ') foreign DOCA/DPDK process(es)"
fi

# HUP as well as the rest: when fleet.py's per-machine timeout fires it kills the local ssh client,
# and what reaches this script is the session hanging up. Without HUP here a timed-out run would
# leave an ib_write_bw pair and a loaded DPA program behind on the machine.
trap cleanup EXIT INT TERM HUP

# --- phases ---------------------------------------------------------------------------------------
phase_setup() {
	log "   running setup_roce_loopback.sh (deletes every OVS bridge and SF on both PFs)"
	if ! sudo -n "$SCRIPTS/setup_roce_loopback.sh" >"$SETUP_LOG" 2>&1; then
		show_tail "$SETUP_LOG"
		STEP_DETAIL="setup_roce_loopback.sh failed"
		return 1
	fi
	STEP_DETAIL="ns0/ns1 up, mlx5_2 + mlx5_3 created"
}

phase_build() {
	# dpacc builds the DPA half of the PCC app. Both pkgconfig shapes are needed: DOCA's .pc files
	# sit under an arch-specific libdir, but flexio's are one level up — miss that and doca-pcc.pc
	# fails to resolve its `Requires.private: libflexio`.
	export PATH="/opt/mellanox/doca/tools:$PATH"
	local extra
	extra=$(ls -d /opt/mellanox/*/lib/pkgconfig /opt/mellanox/*/lib/*/pkgconfig 2>/dev/null | paste -sd: - || true)
	[ -n "$extra" ] && export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$extra"

	if [ "$SKIP_BUILD" -eq 1 ]; then
		[ -x "$FLOW_BIN" ] || { STEP_DETAIL="--skip-build, but ${FLOW_BIN} does not exist"; return 1; }
		BUILD_OK=1
		STEP_DETAIL="reused ${BUILD_DIR} (--skip-build)"
		return 0
	fi

	# From scratch every time. The PCC device code is compiled by build_device_code.sh at meson
	# *configure* time, so an incremental ninja would silently keep a stale DPA image.
	log "   rm -rf ${BUILD_DIR}"
	rm -rf "$BUILD_DIR" || { STEP_DETAIL="could not remove ${BUILD_DIR}"; return 1; }

	log "   meson setup + ninja (this takes a few minutes; log: ${BUILD_LOG})"
	{
		echo "### meson setup"
		( cd "$REPO/$VERSION_DIR" && meson setup build ) 2>&1
	} >"$BUILD_LOG" || { show_tail "$BUILD_LOG" 40; STEP_DETAIL="meson setup failed"; return 1; }
	{
		echo "### ninja"
		ninja -C "$BUILD_DIR" 2>&1
	} >>"$BUILD_LOG" || { show_tail "$BUILD_LOG" 40; STEP_DETAIL="ninja failed"; return 1; }

	[ -x "$FLOW_BIN" ] || { show_tail "$BUILD_LOG" 40; STEP_DETAIL="build produced no ${FLOW_BIN##*/}"; return 1; }
	if [ -z "$PCC_SKIP_REASON" ] && [ ! -x "$PCC_BIN" ]; then
		show_tail "$BUILD_LOG" 40
		STEP_DETAIL="build produced no ${PCC_BIN##*/}"
		return 1
	fi
	BUILD_OK=1
	STEP_DETAIL="${VERSION_DIR}/build"
}

phase_flow_app() {
	sudo -n rm -f "$PCAP" || true
	: >"$FLOW_LOG"
	log "   ${FLOW_BIN##*/} -- --pcap ${PCAP} --sample ${SAMPLE} --percent ${PERCENT}"
	# The `--` is not optional: doca_argp splits EAL arguments from application arguments there.
	sudo -n "$FLOW_BIN" -- --pcap "$PCAP" --sample "$SAMPLE" --percent "$PERCENT" >"$FLOW_LOG" 2>&1 </dev/null &

	if ! wait_for_line "$FLOW_LOG" 'Marking (ALL|NONE|~)' 60; then
		show_tail "$FLOW_LOG"
		STEP_DETAIL="the flow program never reached its run loop"
		return 1
	fi
	FLOW_PID=$(real_pid "$FLOW_BIN") || { STEP_DETAIL="the flow program exited immediately after starting"; return 1; }
	STEP_DETAIL="pid ${FLOW_PID}, capture paused"
}

phase_traffic() {
	# Server first. run_client.sh dials 10.0.0.1 and needs something listening there.
	: >"$SERVER_LOG"
	"$REPO/run_server.sh" >"$SERVER_LOG" 2>&1 </dev/null &
	SERVER_LAUNCHER=$!
	if ! wait_for_line "$SERVER_LOG" 'Waiting for client|local address|Device' 30; then
		show_tail "$SERVER_LOG"
		STEP_DETAIL="run_server.sh never came up"
		return 1
	fi

	: >"$CLIENT_LOG"
	"$REPO/run_client.sh" >"$CLIENT_LOG" 2>&1 </dev/null &
	CLIENT_LAUNCHER=$!
	if ! wait_for_line "$CLIENT_LOG" 'remote address|Device|BW average' 30; then
		show_tail "$CLIENT_LOG"
		STEP_DETAIL="run_client.sh never connected to the server"
		return 1
	fi
	STEP_DETAIL="ib_write_bw pair connected (ns1 -> ns0)"
}

phase_packets_flowing() {
	# The flow program prints a counter line once a second. Two reads WARMUP apart: nonzero proves
	# the eSwitch is marking, increasing proves it is still happening rather than a stale total.
	local before after
	before=$(sed -n 's/.*CE marked: \([0-9]*\).*/\1/p' "$FLOW_LOG" | tail -1)
	log "   settling for ${WARMUP}s"
	sleep "$WARMUP"
	after=$(sed -n 's/.*CE marked: \([0-9]*\).*/\1/p' "$FLOW_LOG" | tail -1)

	if [ -z "${after:-}" ]; then
		show_tail "$FLOW_LOG"
		STEP_DETAIL="the flow program printed no counters"
		return 1
	fi
	if [ "$after" -eq 0 ]; then
		show_tail "$FLOW_LOG"
		STEP_DETAIL="0 packets CE-marked — nothing is reaching PF0 from the wire"
		return 1
	fi
	if [ "$after" -le "${before:-0}" ]; then
		show_tail "$FLOW_LOG"
		STEP_DETAIL="CE counter stuck at ${after} — traffic stopped"
		return 1
	fi
	CE_MARKED="$after"
	STEP_DETAIL="CE marked ${before:-0} -> ${after}"
}

phase_baseline() {
	log "   averaging ${WINDOW}s of throughput"
	sleep "$WINDOW"
	BW_BASE=$(traffic_mean "$WINDOW") || {
		show_tail "$SERVER_LOG"
		STEP_DETAIL="no throughput samples in either ib_write_bw log"
		return 1
	}
	if ! awk -v v="$BW_BASE" -v min="$MIN_GBPS" 'BEGIN { exit !(v >= min) }'; then
		STEP_DETAIL="${BW_BASE} Gb/s is below the ${MIN_GBPS} Gb/s floor"
		return 1
	fi
	STEP_DETAIL="${BW_BASE} Gb/s"
}

phase_capture() {
	# No tty here (fleet.py pipes this script in over ssh), so SPACE cannot be read. SIGUSR1 is the
	# same toggle by another door; the app says so itself at startup.
	local pid
	pid=$(real_pid "$FLOW_BIN") || { STEP_DETAIL="the flow program is gone"; return 1; }
	sudo -n kill -USR1 "$pid" || { STEP_DETAIL="could not signal the flow program"; return 1; }
	if ! wait_for_line "$FLOW_LOG" '\[toggle\] pcap writing ENABLED' 10; then
		show_tail "$FLOW_LOG"
		STEP_DETAIL="SIGUSR1 did not enable pcap writing"
		return 1
	fi

	log "   capturing for ${CAPTURE}s"
	sleep "$CAPTURE"
	sudo -n kill -USR1 "$pid" || true
	sleep 2 # the program flushes on its one-second tick

	local stats ipv4 ce
	stats=$("$SCRIPTS/pcap_ecn_stats.py" --emit "$PCAP" 2>&1) || {
		log "   ${stats}"
		STEP_DETAIL="could not read ${PCAP}"
		return 1
	}
	printf '%s\n' "$stats" | sed 's/^/   /'
	ipv4=$(printf '%s\n' "$stats" | sed -n 's/^@@pcap_ipv4=//p')
	ce=$(printf '%s\n' "$stats" | sed -n 's/^@@pcap_ce=//p')
	ECN_COUNTS="${ce}/${ipv4}"

	if [ "${ipv4:-0}" -lt 10 ]; then
		STEP_DETAIL="only ${ipv4:-0} IPv4 packets captured — try a smaller --sample than ${SAMPLE}"
		return 1
	fi
	# At --percent 100 every marked packet should be CE; the handful that are not would be traffic
	# that entered PF0 by some other path. Below 100 the share is by design, so only require some.
	if awk -v p="$PERCENT" 'BEGIN { exit !(p >= 100) }'; then
		if ! awk -v ce="$ce" -v n="$ipv4" 'BEGIN { exit !(ce >= 0.9 * n) }'; then
			STEP_DETAIL="only ${ce}/${ipv4} captured packets are CE-marked, expected ~all at --percent 100"
			return 1
		fi
	elif [ "${ce:-0}" -eq 0 ]; then
		STEP_DETAIL="no CE-marked packets in the capture"
		return 1
	fi
	STEP_DETAIL="${ce}/${ipv4} captured IPv4 packets are CE-marked"
}

phase_pcc() {
	: >"$PCC_LOG"
	log "   ${PCC_BIN##*/} -d ${PCC_DEV} (traffic keeps running)"
	sudo -n "$PCC_BIN" -d "$PCC_DEV" >"$PCC_LOG" 2>&1 </dev/null &

	if ! wait_for_line "$PCC_LOG" 'starting DOCA PCC' 30; then
		show_tail "$PCC_LOG"
		STEP_DETAIL="the PCC controller never started on ${PCC_DEV}"
		return 1
	fi
	# It exits on its own if USER_PROGRAMMABLE_CC is not live, a second or two after printing that
	# line, so the liveness check has to come after a pause rather than immediately.
	sleep 3
	if ! real_pid "$PCC_BIN" >/dev/null; then
		show_tail "$PCC_LOG"
		STEP_DETAIL="the PCC controller exited right after loading"
		return 1
	fi
	STEP_DETAIL="loaded on ${PCC_DEV}"
}

phase_rate_collapse() {
	log "   settling for ${SETTLE}s, then averaging ${WINDOW}s"
	sleep $((SETTLE + WINDOW))
	BW_PCC=$(traffic_mean "$WINDOW") || {
		show_tail "$SERVER_LOG"
		STEP_DETAIL="no throughput samples after loading PCC"
		return 1
	}
	local threshold
	threshold=$(awk -v b="$BW_BASE" -v r="$MAX_RATIO" 'BEGIN { printf "%.2f", b * r }')
	if ! awk -v v="$BW_PCC" -v t="$threshold" 'BEGIN { exit !(v <= t) }'; then
		STEP_DETAIL="${BW_BASE} -> ${BW_PCC} Gb/s, needed <= ${threshold} — PCC is not throttling"
		return 1
	fi
	STEP_DETAIL="${BW_BASE} -> ${BW_PCC} Gb/s (<= ${threshold})"
}

run_phases() {
	step "roce loopback setup" phase_setup           || return 1
	step "build"               phase_build           || return 1
	step "flow program"        phase_flow_app        || return 1
	step "roce traffic"        phase_traffic         || return 1
	step "packets flowing"     phase_packets_flowing || return 1
	step "baseline throughput" phase_baseline        || return 1
	step "pcap ecn marking"    phase_capture         || return 1
	[ -n "$PCC_SKIP_REASON" ] && return 0
	step "pcc controller"      phase_pcc             || return 1
	step "rate collapse"       phase_rate_collapse   || return 1
}

# --- run ------------------------------------------------------------------------------------------
BUILD_OK=0
CE_MARKED=""
ECN_COUNTS=""
BW_BASE=""
BW_PCC=""
FLOW_PID=""

PHASES_OK=1
run_phases || PHASES_OK=0

# `|| true` so a cleanup step that cannot find something to kill never costs us the summary below,
# which is the only part of this output most people will read.
cleanup || true

# --- report -----------------------------------------------------------------------------------------
log ""
log "== summary =="
FIRST_FAILURE=""
for i in "${!STEP_NAMES[@]}"; do
	printf '   %-6s %-22s %s\n' "${STEP_STATES[$i]}" "${STEP_NAMES[$i]}" "${STEP_DETAILS[$i]}"
	if [ "${STEP_STATES[$i]}" = "FAIL" ] && [ -z "$FIRST_FAILURE" ]; then
		FIRST_FAILURE="${STEP_NAMES[$i]}: ${STEP_DETAILS[$i]}"
	fi
done
[ -n "$PCC_SKIP_REASON" ] && printf '   %-6s %-22s %s\n' "skip" "pcc controller" "$PCC_SKIP_REASON"

# A voluntary --skip-pcc is not a failure; firmware knobs that were never set are, because that
# machine cannot run half the tutorial.
if [ "$PHASES_OK" -eq 1 ] && { [ -z "$PCC_SKIP_REASON" ] || [ "$SKIP_PCC" -eq 1 ]; }; then
	VERDICT=pass
else
	VERDICT=fail
fi

emit build   "$([ "$BUILD_OK" -eq 1 ] && echo ok || echo fail)"
emit flow    "$([ -n "$CE_MARKED" ] && echo ok || echo fail)"
emit ce      "${CE_MARKED:--}"
emit ecn     "${ECN_COUNTS:--}"
emit bw_base "${BW_BASE:--}"
emit bw_pcc  "${BW_PCC:--}"
if [ -n "$PCC_SKIP_REASON" ]; then
	emit pcc "skipped"
	emit pcc_skip_reason "$PCC_SKIP_REASON"
elif [ -n "$BW_PCC" ] && [ "$PHASES_OK" -eq 1 ]; then
	emit pcc "ok"
else
	emit pcc "fail"
fi
emit verdict "$VERDICT"

if [ "$VERDICT" = "pass" ]; then
	SUBJECT="${VERSION_DIR}"
	[ -n "$BW_PCC" ] && SUBJECT="${SUBJECT}, ${BW_BASE} -> ${BW_PCC} Gb/s"
	[ -n "$PCC_SKIP_REASON" ] && SUBJECT="${SUBJECT}, pcc skipped"
else
	SUBJECT="${FIRST_FAILURE:-$PCC_SKIP_REASON}"
fi
emit subject "$SUBJECT"

log ""
log "verdict: ${VERDICT} — ${SUBJECT}"
[ "$VERDICT" = "pass" ]
