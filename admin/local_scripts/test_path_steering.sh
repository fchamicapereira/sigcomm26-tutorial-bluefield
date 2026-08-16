#!/usr/bin/env bash
# Destructive end-to-end test for the dual-receiver PCC path-steering example.
set -euo pipefail

REPO="${TUTORIAL_REPO:-/home/s26t/sigcomm26-tutorial-bluefield}"
LOGDIR="${PATH_STEERING_TEST_LOGDIR:-}"
WARMUP=15
WINDOW=10
MIN_GBPS=10
MIN_CE_RATIO=1.5
MAX_CE_RATIO=2.5
MIN_FLOW_RATIO=0.8
MAX_FLOW_RATIO=1.25
MIN_PATH_RATIO=1.0
MAX_PATH_RATIO=2.0
PCC_DEV=""
SKIP_BUILD=0
SKIP_SETUP=0
FORCE=0
EMIT=0
PF1_PCI="${PF1_PCI:-0000:03:00.1}"

usage() {
	cat <<'EOF'
Usage: test_path_steering.sh [options]

DESTRUCTIVE: replaces every OVS bridge and SF on PF0/PF1 with the three-SF
path-steering topology, then runs two line-rate RDMA workloads.

  --repo DIR         tutorial checkout
  --warmup SEC       traffic/mapping warmup (default: 15)
  --window SEC       measurement window (default: 10)
  --min-gbps X       minimum throughput required from each flow (default: 10)
  --pcc-dev NAME     PCC IB device (default: PF1's device)
  --skip-build       reuse pcc-path-steering/build
  --skip-setup       reuse and validate the existing SF/namespace topology
  --force            ignore an already-running DOCA/DPDK workload
  --emit             emit @@key=value results for fleet.py
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--repo) REPO="$2"; shift 2 ;;
		--warmup) WARMUP="$2"; shift 2 ;;
		--window) WINDOW="$2"; shift 2 ;;
		--min-gbps) MIN_GBPS="$2"; shift 2 ;;
		--pcc-dev) PCC_DEV="$2"; shift 2 ;;
		--skip-build) SKIP_BUILD=1; shift ;;
		--skip-setup) SKIP_SETUP=1; shift ;;
		--force) FORCE=1; shift ;;
		--emit) EMIT=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
done

log() { printf '%s\n' "$*"; }
emit() { [ "$EMIT" -eq 1 ] && printf '@@%s=%s\n' "$1" "$2" || true; }
die() { echo "ERROR: $*" >&2; exit 1; }
show_tail() {
	local file=$1 lines=${2:-30}
	[ -s "$file" ] || return 0
	log "   --- last ${lines} lines of ${file}:"
	tail -n "$lines" "$file" | sed 's/^/   | /'
}
wait_for_line() {
	local file=$1 pattern=$2 timeout=$3 waited=0
	while [ "$waited" -lt "$timeout" ]; do
		grep -Eq -- "$pattern" "$file" 2>/dev/null && return 0
		sleep 1; waited=$((waited + 1))
	done
	return 1
}
real_pid() {
	local binary=$1 p
	for p in $(pgrep -f "$binary" 2>/dev/null || true); do
		[ "$(cat "/proc/$p/comm" 2>/dev/null || true)" = sudo ] && continue
		printf '%s\n' "$p"; return 0
	done
	return 1
}
bw_mean() {
	awk -v n="$2" '
		$0 ~ /^[[:space:]]*[0-9]+([[:space:]]+[0-9.]+){4}[[:space:]]*$/ {v[++c]=$4}
		END {if (!c) exit 1; s=c-n+1; if(s<1)s=1; for(i=s;i<=c;i++)t+=v[i]; printf "%.2f\n",t/(c-s+1)}' "$1"
}
ingress_bw_mean() { # ingress_bw_mean <log> <last-N samples>
	awk -v n="$2" '
		/ingress throughput: path0=/ {
			p0 = p1 = ""
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^path0=[0-9.]+$/) {p0=$i; sub(/^path0=/,"",p0)}
				if ($i ~ /^path1=[0-9.]+$/) {p1=$i; sub(/^path1=/,"",p1)}
			}
			if (p0 != "" && p1 != "") {a0[++c]=p0; a1[c]=p1}
		}
		END {
			if (!c) exit 1
			s=c-n+1; if(s<1)s=1
			for(i=s;i<=c;i++){t0+=a0[i];t1+=a1[i]}
			printf "%.3f %.3f\n",t0/(c-s+1),t1/(c-s+1)
		}' "$1"
}
ce_delta_sum() { # ce_delta_sum <log> <last-N samples>
	awk -v n="$2" '
		/ingress newly CE-marked: path0=/ {
			d0 = d1 = ""; seen = 0
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^\(\+[0-9]+,$/) {
					v=$i; gsub(/[^0-9]/,"",v)
					if (++seen == 1) d0=v; else if (seen == 2) d1=v
				}
			}
			if (d0 != "" && d1 != "") {a0[++c]=d0; a1[c]=d1}
		}
		END {
			if (!c) exit 1
			s=c-n+1; if(s<1)s=1
			for(i=s;i<=c;i++){t0+=a0[i];t1+=a1[i]}
			printf "%.0f %.0f\n",t0,t1
		}' "$1"
}

STEP_NAMES=(); STEP_STATES=(); STEP_DETAILS=(); STEP_DETAIL=""
step() {
	local name=$1; shift; STEP_DETAIL=""; log ""; log "== $name =="
	if "$@"; then STEP_NAMES+=("$name"); STEP_STATES+=(ok); STEP_DETAILS+=("$STEP_DETAIL"); log "-- ok${STEP_DETAIL:+: $STEP_DETAIL}"; return 0; fi
	STEP_NAMES+=("$name"); STEP_STATES+=(FAIL); STEP_DETAILS+=("$STEP_DETAIL"); log "-- FAIL${STEP_DETAIL:+: $STEP_DETAIL}"; return 1
}

if [ -n "$LOGDIR" ]; then
	mkdir -p "$LOGDIR" || die "cannot create $LOGDIR"
else
	LOGDIR=$(mktemp -d /tmp/test_path_steering.XXXXXXXX) || die "cannot create log directory"
fi
SETUP_LOG="$LOGDIR/setup.log"; BUILD_LOG="$LOGDIR/build.log"
INGRESS_LOG="$LOGDIR/ingress.log"; PCC_LOG="$LOGDIR/pcc.log"
P0_SERVER_LOG="$LOGDIR/path0-server.log"; P1_SERVER_LOG="$LOGDIR/path1-server.log"
P0_CLIENT_LOG="$LOGDIR/path0-client.log"; P1_CLIENT_LOG="$LOGDIR/path1-client.log"

PROJECT=""
BUILD_DIR=""
FLOW_BIN=""
PCC_BIN=""
SETUP="$REPO/admin/local_scripts/setup_path_steering.sh"

CLEANUP_DONE=0
cleanup() {
	[ "$CLEANUP_DONE" -eq 1 ] && return 0; CLEANUP_DONE=1
	log ""; log "== cleanup =="
	sudo -n pkill -INT -f "$PCC_BIN" 2>/dev/null || true
	sudo -n pkill -INT -f "$FLOW_BIN" 2>/dev/null || true
	sudo -n pkill -INT -x ib_write_bw 2>/dev/null || true
	local waited=0
	while [ "$waited" -lt 15 ] && pgrep -f "$PCC_BIN|$FLOW_BIN|ib_write_bw" >/dev/null 2>&1; do sleep 1; waited=$((waited+1)); done
	sudo -n pkill -f "$PCC_BIN" 2>/dev/null || true
	sudo -n pkill -f "$FLOW_BIN" 2>/dev/null || true
	sudo -n pkill -x ib_write_bw 2>/dev/null || true
	log "   logs left in $LOGDIR"
}

log "== preflight =="; log "   repo: $REPO"; log "   logs: $LOGDIR"
[ -d "$REPO" ] || die "tutorial checkout missing"
[ -x "$SETUP" ] || die "$SETUP is missing"
DOCA_VERSION=$("$REPO/admin/local_scripts/print_doca_version.sh" 2>/dev/null) || die "cannot determine DOCA version"
emit doca "$DOCA_VERSION"
case "$DOCA_VERSION" in
	2.9*) VERSION_DIR=doca-2 ;;
	3.*) VERSION_DIR=doca-3 ;;
	*) emit verdict unsupported; emit subject "path steering does not target DOCA $DOCA_VERSION"; exit 1 ;;
esac
PROJECT="$REPO/$VERSION_DIR/pcc-path-steering"
BUILD_DIR="$PROJECT/build"
FLOW_BIN="$BUILD_DIR/doca_flow_steer"
PCC_BIN="$BUILD_DIR/doca_pcc"
[ -f "$PROJECT/meson.build" ] || die "$PROJECT is absent; run git submodule update --init --recursive"
log "   doca: $DOCA_VERSION -> $VERSION_DIR"
for command in meson ninja ib_write_bw pgrep stdbuf; do command -v "$command" >/dev/null || die "$command is missing"; done
[ -x /opt/mellanox/doca/tools/dpacc ] || die "dpacc is missing"
sudo -n true 2>/dev/null || die "passwordless sudo is required"
if [ -z "$PCC_DEV" ]; then PCC_DEV=$(ls "/sys/bus/pci/devices/$PF1_PCI/infiniband" 2>/dev/null | head -1 || true); PCC_DEV=${PCC_DEV:-mlx5_1}; fi
pcc_verdict=$("$REPO/admin/local_scripts/check_pcc_ready.sh" --emit 2>/dev/null | sed -n 's/^@@verdict=//p' | head -1 || true)
[ "$pcc_verdict" = ready ] || die "PCC firmware knobs are not live (${pcc_verdict:-unknown})"
BUSY=$(pgrep -af 'doca_|ib_(write|send|read)_bw|testpmd' 2>/dev/null | grep -v -- ' sudo ' || true)
if [ -n "$BUSY" ] && [ "$FORCE" -eq 0 ]; then emit verdict busy; emit subject "another DOCA/DPDK workload is running"; printf '%s\n' "$BUSY"; exit 1; fi
trap cleanup EXIT INT TERM HUP

phase_setup() {
	if [ "$SKIP_SETUP" -eq 1 ]; then
		local spec ns rdma_dev netdev address actual
		for spec in 'ns0 mlx5_2 enp3s0f0s0 10.0.0.1' 'ns1 mlx5_3 enp3s0f1s0 10.0.0.2' 'ns0_1 mlx5_4 enp3s0f0s4 10.0.0.11'; do
			read -r ns rdma_dev netdev address <<<"$spec"
			sudo -n ip netns exec "$ns" rdma dev show "$rdma_dev" >/dev/null 2>&1 || { STEP_DETAIL="$ns has no $rdma_dev"; return 1; }
			actual=$(sudo -n ip -n "$ns" -4 -br addr show "$netdev" 2>/dev/null | awk '{print $3}' | cut -d/ -f1)
			[ "$actual" = "$address" ] || { STEP_DETAIL="$ns/$netdev has ${actual:-no IPv4}, expected $address"; return 1; }
		done
		[ -e /sys/class/net/en3f0pf0sf0 ] && [ -e /sys/class/net/en3f0pf0sf4 ] && [ -e /sys/class/net/en3f1pf1sf0 ] || { STEP_DETAIL="one or more SF representors are missing"; return 1; }
		STEP_DETAIL="validated existing ns0/mlx5_2, ns1/mlx5_3 and ns0_1/mlx5_4 (--skip-setup)"
		return 0
	fi
	sudo -n "$SETUP" --repo "$REPO" >"$SETUP_LOG" 2>&1 || { show_tail "$SETUP_LOG" 50; STEP_DETAIL="dual-path setup failed"; return 1; }
	STEP_DETAIL="ns0/mlx5_2, ns1/mlx5_3 and ns0_1/mlx5_4 ready"
}
phase_build() {
	export PATH="/opt/mellanox/doca/tools:$PATH"
	local extra; extra=$(ls -d /opt/mellanox/*/lib/pkgconfig /opt/mellanox/*/lib/*/pkgconfig 2>/dev/null | paste -sd: - || true)
	[ -n "$extra" ] && export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$extra"
	if [ "$SKIP_BUILD" -eq 0 ]; then
		rm -rf "$BUILD_DIR" || return 1
		meson setup "$BUILD_DIR" "$PROJECT" >"$BUILD_LOG" 2>&1 || { show_tail "$BUILD_LOG" 50; STEP_DETAIL="meson setup failed"; return 1; }
		ninja -C "$BUILD_DIR" >>"$BUILD_LOG" 2>&1 || { show_tail "$BUILD_LOG" 50; STEP_DETAIL="ninja failed"; return 1; }
	fi
	[ -x "$FLOW_BIN" ] && [ -x "$PCC_BIN" ] || { STEP_DETAIL="build did not produce both binaries"; return 1; }
	STEP_DETAIL="$BUILD_DIR"
}
phase_ingress() {
	: >"$INGRESS_LOG"
	sudo -n "$FLOW_BIN" -r pci/0000:03:00.0,pf0sf0 -R pci/0000:03:00.0,pf0sf4 \
		--path0-ip 10.0.0.1 --path1-ip 10.0.0.11 --role ingress \
		--path0-percent 0.025 --path1-percent 0.05 >"$INGRESS_LOG" 2>&1 </dev/null &
	wait_for_line "$INGRESS_LOG" 'steer started \(role=ingress, path0=0\.025% path1=0\.05%\)' 60 || { show_tail "$INGRESS_LOG"; STEP_DETAIL="ingress steering did not start"; return 1; }
	real_pid "$FLOW_BIN" >/dev/null || { STEP_DETAIL="ingress steering exited"; return 1; }
	STEP_DETAIL="0.025% path0, 0.05% path1"
}
phase_pcc() {
	: >"$PCC_LOG"
	sudo -n "$PCC_BIN" --device "$PCC_DEV" -r pci/0000:03:00.1,pf1sf0 \
		--path0-ip 10.0.0.1 --path1-ip 10.0.0.11 >"$PCC_LOG" 2>&1 </dev/null &
	wait_for_line "$PCC_LOG" 'steer started \(role=egress' 60 || { show_tail "$PCC_LOG"; STEP_DETAIL="PCC embedded egress did not start"; return 1; }
	sleep 3; real_pid "$PCC_BIN" >/dev/null || { show_tail "$PCC_LOG"; STEP_DETAIL="PCC exited after startup"; return 1; }
	STEP_DETAIL="PCC on $PCC_DEV with embedded egress steering"
}
phase_traffic() {
	: >"$P0_SERVER_LOG"; : >"$P1_SERVER_LOG"; : >"$P0_CLIENT_LOG"; : >"$P1_CLIENT_LOG"
	sudo -n ip netns exec ns0 stdbuf -oL ib_write_bw -d mlx5_2 -R --report_gbits --run_infinitely >"$P0_SERVER_LOG" 2>&1 </dev/null &
	sudo -n ip netns exec ns0_1 stdbuf -oL ib_write_bw -d mlx5_4 -R --report_gbits --run_infinitely >"$P1_SERVER_LOG" 2>&1 </dev/null &
	wait_for_line "$P0_SERVER_LOG" 'Waiting for client|local address' 30 && wait_for_line "$P1_SERVER_LOG" 'Waiting for client|local address' 30 || { STEP_DETAIL="one receiver did not start"; return 1; }
	sudo -n ip netns exec ns1 stdbuf -oL ib_write_bw -d mlx5_3 -R -F 10.0.0.1 --report_gbits --run_infinitely >"$P0_CLIENT_LOG" 2>&1 </dev/null &
	sudo -n ip netns exec ns1 stdbuf -oL ib_write_bw -d mlx5_3 -R -F 10.0.0.11 --report_gbits --run_infinitely >"$P1_CLIENT_LOG" 2>&1 </dev/null &
	wait_for_line "$P0_CLIENT_LOG" 'remote address|BW average' 30 && wait_for_line "$P1_CLIENT_LOG" 'remote address|BW average' 30 || { show_tail "$P0_CLIENT_LOG"; show_tail "$P1_CLIENT_LOG"; STEP_DETAIL="one sender did not connect"; return 1; }
	STEP_DETAIL="both RDMA-CM flows connected"
}
phase_mapping() {
	log "   warming up for ${WARMUP}s"; sleep "$WARMUP"
	case "$DOCA_VERSION" in
		2.*)
			# DOCA 2 mirrors every UDP/4791 packet because its public Flow API
			# cannot match BTH QPN. Under load QP1 clones may be dropped, so its
			# supported fallback learns sender QPN -> path from feedback source IP.
			if grep -Eq 'RDMA-CM mapping: sender 0x[0-9a-fA-F]+ -> receiver unknown path0 \(DOCA 2 ingress-feedback inference\)' "$PCC_LOG" &&
			   grep -Eq 'RDMA-CM mapping: sender 0x[0-9a-fA-F]+ -> receiver unknown path1 \(DOCA 2 ingress-feedback inference\)' "$PCC_LOG"; then
				QPN_MAP=feedback
			elif grep -Eq 'ingress feedback grouping: .* -> path0' "$PCC_LOG" &&
			     grep -Eq 'ingress feedback grouping: .* -> path1' "$PCC_LOG"; then
				QPN_MAP=feedback
			elif grep -Eq 'PCC path-share diagnostic: path0 total=[1-9][0-9]* .*; path1 total=[1-9][0-9]* ' "$PCC_LOG"; then
				# The diagnostic is authoritative steady-state evidence that both
				# groups were populated even if their one-time learn logs rolled out.
				QPN_MAP=feedback
			else
				show_tail "$PCC_LOG" 80
				STEP_DETAIL="did not learn QPN groups for both paths from DOCA 2 feedback"
				return 1
			fi
			STEP_DETAIL="learned path0 and path1 QPN groups from feedback packets"
			;;
		*)
			grep -Eq 'REQ grouping: .* -> path0' "$PCC_LOG" && grep -Eq 'REQ grouping: .* -> path1' "$PCC_LOG" &&
			grep -Eq 'RDMA-CM mapping: .* path0' "$PCC_LOG" && grep -Eq 'RDMA-CM mapping: .* path1' "$PCC_LOG" || { show_tail "$PCC_LOG" 80; STEP_DETAIL="did not complete RDMA-CM mappings for both destination IPs"; return 1; }
			QPN_MAP=rdma-cm
			STEP_DETAIL="completed path0 and path1 RDMA-CM QPN mappings"
			;;
	esac
}
phase_bandwidth() {
	sleep "$WINDOW"
	BW_PATH0=$(bw_mean "$P0_CLIENT_LOG" "$WINDOW") || { STEP_DETAIL="no path0 throughput samples"; return 1; }
	BW_PATH1=$(bw_mean "$P1_CLIENT_LOG" "$WINDOW") || { STEP_DETAIL="no path1 throughput samples"; return 1; }
	for item in "$BW_PATH0:path0" "$BW_PATH1:path1"; do local value=${item%%:*} path=${item##*:}; awk -v v="$value" -v m="$MIN_GBPS" 'BEGIN{exit !(v>=m)}' || { STEP_DETAIL="$path throughput $value Gb/s is below $MIN_GBPS"; return 1; }; done
	BW_TOTAL=$(awk -v p0="$BW_PATH0" -v p1="$BW_PATH1" 'BEGIN{printf "%.2f",p0+p1}')
	FLOW_RATIO=$(awk -v p0="$BW_PATH0" -v p1="$BW_PATH1" 'BEGIN{printf "%.3f",p0/p1}')
	awk -v r="$FLOW_RATIO" -v lo="$MIN_FLOW_RATIO" -v hi="$MAX_FLOW_RATIO" 'BEGIN{exit !(r>=lo && r<=hi)}' || { STEP_DETAIL="ib_write_bw flow ratio path0/path1=$FLOW_RATIO, expected $MIN_FLOW_RATIO..$MAX_FLOW_RATIO"; return 1; }
	if grep -Eqi 'Bad wc status|transport retry counter exceeded|Failed to exchange|Completion with error' "$P0_CLIENT_LOG" "$P1_CLIENT_LOG"; then STEP_DETAIL="RDMA transport error in a client log"; return 1; fi
	STEP_DETAIL="flow0=${BW_PATH0} Gb/s flow1=${BW_PATH1} Gb/s ratio=${FLOW_RATIO} total=${BW_TOTAL} Gb/s"
}
phase_path_distribution() {
	read -r STEER_BW0 STEER_BW1 < <(ingress_bw_mean "$INGRESS_LOG" "$WINDOW") || { show_tail "$INGRESS_LOG"; STEP_DETAIL="missing ingress path-throughput counters"; return 1; }
	PATH_RATIO=$(awk -v p0="$STEER_BW0" -v p1="$STEER_BW1" 'BEGIN{if(p1<=0)exit 1; printf "%.3f",p0/p1}') || { STEP_DETAIL="path1 ingress throughput is zero"; return 1; }
	STEER_TOTAL=$(awk -v p0="$STEER_BW0" -v p1="$STEER_BW1" 'BEGIN{printf "%.3f",p0+p1}')
	awk -v r="$PATH_RATIO" -v lo="$MIN_PATH_RATIO" -v hi="$MAX_PATH_RATIO" 'BEGIN{exit !(r>=lo && r<=hi)}' || { STEP_DETAIL="steering path0/path1=$PATH_RATIO, expected $MIN_PATH_RATIO..$MAX_PATH_RATIO"; return 1; }
	STEP_DETAIL="path0=${STEER_BW0} Gb/s path1=${STEER_BW1} Gb/s total=${STEER_TOTAL} Gb/s ratio=${PATH_RATIO}"
}
phase_ecn_ratio() {
	local d0 d1
	# Wait for one complete window, then take CE deltas and path bandwidth from
	# the same last-N per-second records. PCC may change the share during this
	# phase, so bandwidth sampled before the sleep cannot normalize these marks.
	sleep "$WINDOW"
	read -r d0 d1 < <(ce_delta_sum "$INGRESS_LOG" "$WINDOW") || { show_tail "$INGRESS_LOG"; STEP_DETAIL="missing ingress CE deltas"; return 1; }
	read -r STEER_BW0 STEER_BW1 < <(ingress_bw_mean "$INGRESS_LOG" "$WINDOW") || { show_tail "$INGRESS_LOG"; STEP_DETAIL="missing ingress path-throughput counters"; return 1; }
	[ "$d0" -gt 0 ] && [ "$d1" -gt 0 ] || { STEP_DETAIL="CE deltas are path0=$d0 path1=$d1"; return 1; }
	PATH_RATIO=$(awk -v p0="$STEER_BW0" -v p1="$STEER_BW1" 'BEGIN{if(p1<=0)exit 1; printf "%.3f",p0/p1}') || { STEP_DETAIL="path1 ingress throughput is zero"; return 1; }
	STEER_TOTAL=$(awk -v p0="$STEER_BW0" -v p1="$STEER_BW1" 'BEGIN{printf "%.3f",p0+p1}')
	CE_RAW_RATIO=$(awk -v a="$d1" -v b="$d0" 'BEGIN{printf "%.3f",a/b}')
	CE_RATIO=$(awk -v c1="$d1" -v c0="$d0" -v bw0="$STEER_BW0" -v bw1="$STEER_BW1" 'BEGIN{printf "%.3f",(c1/bw1)/(c0/bw0)}')
	awk -v r="$CE_RATIO" -v lo="$MIN_CE_RATIO" -v hi="$MAX_CE_RATIO" 'BEGIN{exit !(r>=lo && r<=hi)}' || { STEP_DETAIL="CE ratio path1/path0=$CE_RATIO, expected $MIN_CE_RATIO..$MAX_CE_RATIO"; return 1; }
	CE_PATH0=$d0; CE_PATH1=$d1; STEP_DETAIL="same-window path0=$d0 path1=$d1 raw-ratio=$CE_RAW_RATIO normalized-ratio=$CE_RATIO"
}
phase_share() {
	local waited=0 line new
	while [ "$waited" -lt 45 ]; do
		line=$(grep 'PCC path share applied:' "$PCC_LOG" | tail -1 || true)
		new=$(printf '%s\n' "$line" | sed -n 's/.*path0 [0-9]*->\([0-9]*\)\/64.*/\1/p')
		[ -n "$new" ] && [ "$new" -gt 32 ] && break
		sleep 1; waited=$((waited+1))
	done
	[ -n "${new:-}" ] && [ "$new" -gt 32 ] || { show_tail "$PCC_LOG" 80; STEP_DETAIL="path0 never received the larger applied share"; return 1; }
	APPLIED_PATH0=$new; STEP_DETAIL="applied path0=$new/64 path1=$((64-new))/64"
}

run_phases() {
	step "dual-path setup" phase_setup || return 1
	step "path-steering build" phase_build || return 1
	step "ingress steering" phase_ingress || return 1
	step "pcc egress steering" phase_pcc || return 1
	step "two-path traffic" phase_traffic || return 1
	step "qpn path mapping" phase_mapping || return 1
	step "balanced rdma flows" phase_bandwidth || return 1
	step "steering distribution" phase_path_distribution || return 1
	step "asymmetric ecn" phase_ecn_ratio || return 1
	step "dynamic path share" phase_share || return 1
}

QPN_MAP=""; BW_PATH0=""; BW_PATH1=""; BW_TOTAL=""; FLOW_RATIO=""; STEER_BW0=""; STEER_BW1=""; STEER_TOTAL=""; PATH_RATIO=""; CE_PATH0=""; CE_PATH1=""; CE_RAW_RATIO=""; CE_RATIO=""; APPLIED_PATH0=""
PHASES_OK=1; run_phases || PHASES_OK=0; cleanup || true
log ""; log "== summary =="; FIRST_FAILURE=""
for i in "${!STEP_NAMES[@]}"; do printf '   %-6s %-22s %s\n' "${STEP_STATES[$i]}" "${STEP_NAMES[$i]}" "${STEP_DETAILS[$i]}"; [ "${STEP_STATES[$i]}" = FAIL ] && [ -z "$FIRST_FAILURE" ] && FIRST_FAILURE="${STEP_NAMES[$i]}: ${STEP_DETAILS[$i]}"; done
[ "$PHASES_OK" -eq 1 ] && VERDICT=pass || VERDICT=fail
emit path0_bw "${BW_PATH0:--}"; emit path1_bw "${BW_PATH1:--}"; emit total_bw "${BW_TOTAL:--}"; emit flow_ratio "${FLOW_RATIO:--}"; emit steer_path0_bw "${STEER_BW0:--}"; emit steer_path1_bw "${STEER_BW1:--}"; emit path_ratio "${PATH_RATIO:--}"; emit path0_ce "${CE_PATH0:--}"; emit path1_ce "${CE_PATH1:--}"
emit ce_raw_ratio "${CE_RAW_RATIO:--}"; emit ce_ratio "${CE_RATIO:--}"; emit qpn_map "${QPN_MAP:-fail}"; emit applied_path0 "${APPLIED_PATH0:--}"; emit verdict "$VERDICT"
if [ "$VERDICT" = pass ]; then SUBJECT="flows ${BW_PATH0}/${BW_PATH1} Gb/s; steering paths ${STEER_BW0}/${STEER_BW1} Gb/s (${PATH_RATIO}x); CE/Gb ${CE_RATIO}x; share ${APPLIED_PATH0}/64"; else SUBJECT="${FIRST_FAILURE:-unknown failure}"; fi
emit subject "$SUBJECT"; log ""; log "verdict: $VERDICT — $SUBJECT"; [ "$VERDICT" = pass ]
