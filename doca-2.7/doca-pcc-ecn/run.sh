#!/usr/bin/env bash
#
# Run the doca-pcc-ecn controller (the RP reaction point) on PF1 / mlx5_1, as the README describes.
# Build it first from the repo root: `meson setup build && ninja -C build` (produces
# build/doca-pcc-ecn/doca_pcc_ecn_rp), same as every other program in this repo.
#
# Prerequisites (see the top-level README):
#   - Firmware NV-config USER_PROGRAMMABLE_CC=1 must be LIVE, or doca_pcc_ecn_rp refuses to start.
#   - Hugepages reserved (setup_roce_loopback.sh does this; the container entrypoint runs it).
#   - Point it at the uplink PF (mlx5_1); RoCE traffic itself rides the SF (mlx5_3).
#
# Usage:
#   ./run.sh                 # sudo doca_pcc_ecn_rp -d mlx5_1 -l 50
#   DEV=mlx5_1 LOG=50 ./run.sh
#
# Stop with Ctrl-C (SIGINT). Never SIGKILL it — that leaves a ghost DPA context on the device.
#
set -euo pipefail

# Operate relative to this script's own directory (doca-pcc-ecn/), regardless of caller's CWD.
cd "$(dirname "$(readlink -f "$0")")"

BIN="../build/doca-pcc-ecn/doca_pcc_ecn_rp"
DEV="${DEV:-mlx5_1}"
LOG="${LOG:-50}"

if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found — build it first from the repo root: meson setup build && ninja -C build" >&2
  exit 1
fi

echo ">> sudo $BIN -d $DEV -l $LOG $*"
exec sudo "$BIN" -d "$DEV" -l "$LOG" "$@"
