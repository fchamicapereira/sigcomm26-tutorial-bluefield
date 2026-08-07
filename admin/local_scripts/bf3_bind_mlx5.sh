#!/usr/bin/env bash
# =============================================================================
# bf3_bind_mlx5.sh  —  v5
# Purpose  : Migrate BF3 NIC functions (.0 and .1 only) vfio-pci → mlx5_core
#            so mlxfwreset can run. Auto-reverts on any failure.
# Usage    : sudo bash bf3_bind_mlx5.sh [--dry-run]
# =============================================================================

set -uo pipefail

DRY=false
[[ "${1:-}" == "--dry-run" ]] && DRY=true

FUNCTIONS=(0 1)
MLX5_INIT_TIMEOUT=90

# Per-function state snapshot (populated before any changes)
declare -A SNAP_DRIVER=()
declare -A SNAP_OVERRIDE=()

# Execution log for revert — ordered list of completed step IDs
COMPLETED=()

# Colours
RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'
BLU='\033[0;34m'; CYN='\033[0;36m'; BLD='\033[1m'; NC='\033[0m'

log()   { echo -e "${GRN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YLW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR ]${NC}  $*" >&2; }
step()  { echo -e "\n${BLD}${BLU}──────────────────────────────────────────${NC}"; \
          echo -e "${BLD}${BLU}  $* ${NC}"; \
          echo -e "${BLD}${BLU}──────────────────────────────────────────${NC}"; }

get_driver() {
    readlink "/sys/bus/pci/devices/${1}/driver" 2>/dev/null \
        | awk -F/ '{print $NF}' || echo "unbound"
}

get_override() {
    local val
    val=$(cat "/sys/bus/pci/devices/${1}/driver_override" 2>/dev/null || true)
    echo "${val:-}"
}

show_state() {
    local prefix="$1" label="${2:-State}"
    echo -e "\n  ${CYN}┌── ${label} ──────────────────────────────────────────────────────┐${NC}"
    for func in "${FUNCTIONS[@]}"; do
        local dev="0000:${prefix}.${func}"
        local drv ovr netdev
        drv=$(get_driver "$dev")
        ovr=$(get_override "$dev"); ovr="${ovr:-<none>}"
        netdev=$(ls "/sys/bus/pci/devices/${dev}/net/" 2>/dev/null | paste -sd, || echo "—")
        printf "  ${CYN}│${NC}  %-20s  driver=%-14s  override=%-14s  net=%s\n" \
            "$dev" "$drv" "$ovr" "$netdev"
    done
    local dev2="0000:${prefix}.2"
    [[ -e "/sys/bus/pci/devices/${dev2}" ]] && \
        printf "  ${CYN}│${NC}  %-20s  driver=%-14s  ${YLW}(.2 — not touched)${NC}\n" \
            "$dev2" "$(get_driver "$dev2")"
    echo -e "  ${CYN}└──────────────────────────────────────────────────────────────────┘${NC}"
}

# =============================================================================
# REVERT — runs in reverse order of COMPLETED steps
# =============================================================================
revert() {
    local prefix="$1"
    echo ""
    err "=== FAILURE DETECTED — auto-reverting to snapshot state ==="
    echo ""

    # Reverse the completed list
    local -a rev=()
    for (( i=${#COMPLETED[@]}-1; i>=0; i-- )); do
        rev+=("${COMPLETED[$i]}")
    done

    local revert_ok=true
    for step_id in "${rev[@]}"; do
        case "$step_id" in

            probe_*)
                local func="${step_id#probe_}"
                local dev="0000:${prefix}.${func}"
                warn "Revert probe_${func}: unbind ${dev} from mlx5_core"
                echo "${dev}" > /sys/bus/pci/drivers/mlx5_core/unbind 2>/dev/null \
                    && log "  ${dev} unbound ✓" \
                    || { err "  Failed to unbind ${dev}"; revert_ok=false; }
                ;;

            override_*)
                local func="${step_id#override_}"
                local dev="0000:${prefix}.${func}"
                local orig="${SNAP_OVERRIDE[$func]}"
                warn "Revert override_${func}: restore driver_override='${orig}' on ${dev}"
                echo "${orig}" > "/sys/bus/pci/devices/${dev}/driver_override" 2>/dev/null \
                    && log "  driver_override restored ✓" \
                    || { err "  Failed to restore driver_override on ${dev}"; revert_ok=false; }
                ;;

            unbind_*)
                local func="${step_id#unbind_}"
                local dev="0000:${prefix}.${func}"
                local orig="${SNAP_DRIVER[$func]}"
                if [[ "$orig" == "vfio-pci" ]]; then
                    warn "Revert unbind_${func}: re-bind ${dev} to vfio-pci"
                    echo "${dev}" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null \
                        && log "  ${dev} re-bound to vfio-pci ✓" \
                        || { err "  Failed to re-bind ${dev} to vfio-pci"; revert_ok=false; }
                else
                    warn "Revert unbind_${func}: original driver was '${orig}' — no re-bind needed"
                fi
                ;;

            modprobe_mlx5)
                # Do not unload mlx5_core — other devices on the host may use it
                warn "Revert modprobe_mlx5: mlx5_core left loaded (other devices may depend on it)"
                ;;
        esac
    done

    echo ""
    show_state "$prefix" "State after revert attempt"
    echo ""
    if $revert_ok; then
        warn "Revert completed. Device should be in its original state."
        warn "Verify with: mst status -v"
    else
        err "Revert encountered errors. Manual inspection required."
        err "Run: dmesg | grep -E '${prefix}|vfio|mlx5'"
    fi
    exit 1
}

# =============================================================================
# EXECUTE — each step registers itself in COMPLETED before returning
# =============================================================================
execute_steps() {
    local prefix="$1"

    # ── Step 1: Unbind from vfio-pci ─────────────────────────────────────────
    step "Step 1/4 — Unbind from vfio-pci"
    for func in "${FUNCTIONS[@]}"; do
        local dev="0000:${prefix}.${func}"
        local drv="${SNAP_DRIVER[$func]}"
        if [[ "$drv" == "vfio-pci" ]]; then
            log "Unbinding ${dev} from vfio-pci..."
            if $DRY; then
                log "[dry] echo '${dev}' > /sys/bus/pci/drivers/vfio-pci/unbind"
            else
                echo "${dev}" > /sys/bus/pci/drivers/vfio-pci/unbind \
                    || { err "Failed to unbind ${dev} from vfio-pci"; revert "$prefix"; }
            fi
            COMPLETED+=("unbind_${func}")
            log "  ${dev} unbound ✓"
        else
            log "  ${dev} is '${drv}' — no vfio-pci unbind needed"
            # Still record so revert knows there's nothing to undo here
            COMPLETED+=("unbind_${func}")
        fi
    done

    # ── Step 2: Set driver_override ──────────────────────────────────────────
    step "Step 2/4 — Set driver_override=mlx5_core"
    for func in "${FUNCTIONS[@]}"; do
        local dev="0000:${prefix}.${func}"
        log "Setting driver_override=mlx5_core on ${dev}..."
        if $DRY; then
            log "[dry] echo 'mlx5_core' > /sys/bus/pci/devices/${dev}/driver_override"
        else
            echo "mlx5_core" > "/sys/bus/pci/devices/${dev}/driver_override" \
                || { err "Failed to set driver_override on ${dev}"; revert "$prefix"; }
        fi
        COMPLETED+=("override_${func}")
        log "  ${dev} driver_override=mlx5_core ✓"
    done

    # ── Step 3: Load mlx5_core + probe ───────────────────────────────────────
    step "Step 3/4 — Load mlx5_core and probe"
    if [[ ! -d /sys/module/mlx5_core ]]; then
        log "Loading mlx5_core..."
        if $DRY; then
            log "[dry] modprobe mlx5_core"
        else
            modprobe mlx5_core \
                || { err "modprobe mlx5_core failed"; revert "$prefix"; }
        fi
    else
        log "mlx5_core already loaded."
    fi
    COMPLETED+=("modprobe_mlx5")

    for func in "${FUNCTIONS[@]}"; do
        local dev="0000:${prefix}.${func}"
        log "Probing ${dev}..."
        if $DRY; then
            log "[dry] echo '${dev}' > /sys/bus/pci/drivers_probe"
        else
            echo "${dev}" > /sys/bus/pci/drivers_probe \
                || { err "drivers_probe failed for ${dev}"; revert "$prefix"; }
        fi
        COMPLETED+=("probe_${func}")
        log "  ${dev} probe triggered ✓"
    done

    # ── Step 4: Wait for mlx5 FW init ────────────────────────────────────────
    step "Step 4/4 — Wait for mlx5_core firmware init"
    if ! $DRY; then
        for func in "${FUNCTIONS[@]}"; do
            local dev="0000:${prefix}.${func}"
            local elapsed=0
            echo -n "  ${dev}: waiting"
            while (( elapsed < MLX5_INIT_TIMEOUT )); do
                local netdev
                netdev=$(ls "/sys/bus/pci/devices/${dev}/net/" 2>/dev/null | head -1 || true)
                if [[ -n "$netdev" ]]; then
                    echo ""; log "${dev} → netdev ${netdev} ✓"; break
                fi
                echo -n "."; sleep 2; (( elapsed += 2 ))
            done
            if (( elapsed >= MLX5_INIT_TIMEOUT )); then
                echo ""
                warn "${dev}: no netdev after ${MLX5_INIT_TIMEOUT}s — FW may still be init-ing"
                warn "Check: dmesg | grep ${dev}"
                # Not fatal — FW init continues after probe, mlxfwreset will wait
            fi
        done
    fi

    # ── MST restart ───────────────────────────────────────────────────────────
    if command -v mst &>/dev/null; then
        log "Restarting MST daemon..."
        $DRY && log "[dry] mst restart" || mst restart
        log "MST restarted ✓"
    fi
}

# =============================================================================
# DEVICE DISCOVERY
# =============================================================================
discover_bf3() {
    local -a found=()
    for sysdev in /sys/bus/pci/devices/0000:*/; do
        local v d
        v=$(cat "${sysdev}vendor" 2>/dev/null || true)
        d=$(cat "${sysdev}device" 2>/dev/null || true)
        [[ "$v" == "0x15b3" && "$d" == "0xa2dc" ]] || continue
        local p
        p=$(basename "$sysdev" | grep -oP '\d+:\d+(?=\.\d)')
        [[ ! " ${found[*]:-} " =~ " ${p} " ]] && found+=("$p")
    done
    printf '%s\n' "${found[@]:-}"
}

select_device() {
    step "Discovering BF3 devices"
    local -a bdfs
    mapfile -t bdfs < <(discover_bf3)
    [[ ${#bdfs[@]} -gt 0 ]] || { err "No BF3 NIC PF devices found."; exit 1; }

    echo ""
    printf "  ${BLD}%-4s %-12s %-14s %-14s %-20s %-6s${NC}\n" \
        "NUM" "BDF" ".0 DRIVER" ".1 DRIVER" "MST PATH" "NUMA"
    printf "  %-4s %-12s %-14s %-14s %-20s %-6s\n" \
        "---" "---" "---------" "---------" "--------" "----"

    local i=1
    local -a menu=()
    for p in "${bdfs[@]}"; do
        local d0 d1 mst numa
        d0=$(get_driver "0000:${p}.0")
        d1=$(get_driver "0000:${p}.1")
        numa=$(cat "/sys/bus/pci/devices/0000:${p}.0/numa_node" 2>/dev/null || echo "?")
        mst="—"
        command -v mst &>/dev/null && \
            mst=$(mst status -v 2>/dev/null | grep -i bluefield | grep "${p}\." \
                  | awk '{print $2}' | grep -v '\.' | head -1 || echo "—")
        printf "  ${GRN}[%-2s]${NC} %-12s %-14s %-14s %-20s %-6s\n" \
            "$i" "$p" "$d0" "$d1" "$mst" "$numa"
        menu+=("$p")
        (( i++ ))
    done

    echo ""
    local choice
    while true; do
        read -r -p "  Select device [1-${#menu[@]}] or q to quit: " choice
        [[ "$choice" == "q" ]] && { echo "Aborted."; exit 0; }
        if [[ "$choice" =~ ^[0-9]+$ ]] && (( choice >= 1 && choice <= ${#menu[@]} )); then
            SELECTED="${menu[$((choice-1))]}"; break
        fi
        echo "  Invalid — enter a number between 1 and ${#menu[@]}."
    done
    log "Selected: ${SELECTED}"
}

# =============================================================================
# MAIN
# =============================================================================
$DRY && warn "=== DRY-RUN MODE ==="
[[ $EUID -eq 0 ]] || { err "Must run as root."; exit 1; }

select_device
PREFIX="$SELECTED"

# Validate devices exist and are genuine BF3
for func in "${FUNCTIONS[@]}"; do
    DEV="0000:${PREFIX}.${func}"
    [[ -e "/sys/bus/pci/devices/${DEV}" ]] || { err "${DEV} not in sysfs."; exit 1; }
    V=$(cat "/sys/bus/pci/devices/${DEV}/vendor" 2>/dev/null)
    [[ "$V" == "0x15b3" ]] || { err "${DEV} vendor=${V} — not a Mellanox device."; exit 1; }
done

# VM safety check
if command -v qm &>/dev/null; then
    step "Checking for running VMs"
    while IFS= read -r line; do
        VMID=$(echo "$line" | awk '{print $1}')
        STATUS=$(echo "$line" | awk '{print $3}')
        [[ "$STATUS" != "running" ]] && continue
        CONF="/etc/pve/qemu-server/${VMID}.conf"
        [[ -f "$CONF" ]] && grep -qi "${PREFIX}" "$CONF" 2>/dev/null && \
            { err "VM ${VMID} is running with ${PREFIX} passed through. Stop it first: qm stop ${VMID}"; exit 1; }
    done < <(qm list 2>/dev/null | tail -n +2)
    log "No running VMs using ${PREFIX} ✓"
fi

# Snapshot current per-function state before touching anything
for func in "${FUNCTIONS[@]}"; do
    DEV="0000:${PREFIX}.${func}"
    SNAP_DRIVER[$func]=$(get_driver "$DEV")
    SNAP_OVERRIDE[$func]=$(get_override "$DEV")
done

# Show current state and execution plan
show_state "$PREFIX" "Current state (snapshot)"
echo ""
echo -e "  ${BLD}Execution plan${NC}"
echo -e "  ┌────────────────────────────────────────────────────┐"
echo -e "  │  Target : ${PREFIX}.0 and ${PREFIX}.1 → mlx5_core           │"
echo -e "  │  .2     : not touched                              │"
echo -e "  │  Revert : automatic on any failure                 │"
echo -e "  │  Steps  : unbind vfio → set override → probe       │"
echo -e "  │           → wait fw init → mst restart             │"
echo -e "  └────────────────────────────────────────────────────┘"
echo ""
echo -e "  ${YLW}Snapshot saved. On failure, all executed steps will be reversed.${NC}"
echo ""

if ! $DRY; then
    read -r -p "  Proceed? [yes/N]: " CONFIRM
    [[ "$CONFIRM" == "yes" ]] || { echo "Aborted."; exit 0; }
fi

# Execute — auto-reverts on any failure
execute_steps "$PREFIX"

# Final state
step "Complete"
show_state "$PREFIX" "Final state"
echo ""

ALL_OK=true
for func in "${FUNCTIONS[@]}"; do
    DEV="0000:${PREFIX}.${func}"
    DRV=$(get_driver "$DEV")
    if [[ "$DRV" == "mlx5_core" ]]; then
        log "${DEV} → mlx5_core ✓"
    else
        warn "${DEV} → '${DRV}'"
        ALL_OK=false
    fi
done

if $ALL_OK || $DRY; then
    MST_DEV="—"
    command -v mst &>/dev/null && \
        MST_DEV=$(mst status -v 2>/dev/null | grep -i bluefield \
            | grep "${PREFIX}\." | awk '{print $2}' | grep -v '\.' | head -1 || echo "—")
    echo ""
    log "BF3 is ready for firmware operations."
    echo ""
    echo "  Query available reset levels:"
    echo "    mlxfwreset -d ${MST_DEV} query"
    echo ""
    echo "  Execute reset:"
    echo "    mlxfwreset -d ${MST_DEV} -l 3 -y reset"
    echo ""
    echo "  When done, restore passthrough:"
    echo "    sudo bash bf3_bind_vfio.sh"
else
    err "Not all functions bound to mlx5_core. Check: dmesg | grep -E '${PREFIX}|mlx5'"
    exit 1
fi
