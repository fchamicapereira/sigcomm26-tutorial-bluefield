#!/usr/bin/env bash
#
# Run the doca-pcc-ecn controller (the RP reaction point) on PF1 / mlx5_1, as the README describes.
# Build it first with ./build.sh (produces app/build/pcc/doca_pcc).
#
# Prerequisites (see the top-level README):
#   - Firmware NV-config USER_PROGRAMMABLE_CC=1 must be LIVE, or doca_pcc refuses to start.
#   - Hugepages reserved (setup/setup_roce_loopback.sh does this; the container entrypoint runs it).
#   - Point doca_pcc at the uplink PF (mlx5_1); RoCE traffic itself rides the SF (mlx5_3).
#
# Usage:
#   ./run.sh                 # sudo doca_pcc -d mlx5_1 -l 50
#   DEV=mlx5_1 LOG=50 ./run.sh
#   ./run.sh -np-nt          # extra flags are appended verbatim (e.g. NP notification-point mode)
#
# Stop with Ctrl-C (SIGINT). Never SIGKILL it — that leaves a ghost DPA context on the device.
#
set -euo pipefail

# Operate relative to this script's own directory (doca-pcc-ecn/), regardless of caller's CWD.
cd "$(dirname "$(readlink -f "$0")")"

BIN="app/build/pcc/doca_pcc"
DEV="${DEV:-mlx5_1}"
LOG="${LOG:-50}"

if [[ ! -x "$BIN" ]]; then
  echo "ERROR: $BIN not found — build it first with ./build.sh" >&2
  exit 1
fi

echo ">> sudo $BIN -d $DEV -l $LOG $*"
exec sudo "$BIN" -d "$DEV" -l "$LOG" "$@"
