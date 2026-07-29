#!/usr/bin/env bash
#
# Build the doca-pcc-ecn controller — NVIDIA's stock rtt_template DOCA PCC app with our pure-ECN
# (DCQCN-style) patch applied — exactly as the top-level README's "Building and running DOCA PCC
# (Part IV)" section describes. Produces: doca-pcc-ecn/app/build/pcc/doca_pcc
#
# This is a CLEAN rebuild: it regenerates the git-ignored app/ tree from the DOCA install + the
# patch every time, so it's reproducible. Runs wherever DOCA lives under /opt/mellanox/doca —
# natively on the Arm, or inside the tutorial devel container (the PCC build is intentionally a
# runtime step, not baked into the image; see the Dockerfile). Override with DOCA=/path ./build.sh
#
# Usage:
#   ./build.sh
#
set -euo pipefail

# Operate relative to this script's own directory (doca-pcc-ecn/), regardless of caller's CWD.
cd "$(dirname "$(readlink -f "$0")")"

DOCA="${DOCA:-/opt/mellanox/doca}"
PATCH="pureecn_dcqcn.patch"

if [[ ! -x "${DOCA}/tools/dpacc" ]]; then
  echo "ERROR: dpacc not found at ${DOCA}/tools/dpacc — is DOCA installed, or are you inside the" >&2
  echo "       devel container? (Set DOCA=/path if your install prefix differs.)" >&2
  exit 1
fi

echo ">> [1/4] fresh writable copy of ${DOCA}/applications -> ./app (git-ignored)"
rm -rf app
cp -a "${DOCA}/applications" app
rm -rf app/build   # /opt ships a prebuilt build/ — drop it so we configure our own from scratch

# The applications meson.build reads its version from ../VERSION, which in /opt lives ONE level
# ABOVE the applications tree — so `cp -a applications` leaves it behind. Drop a copy next to app/
# or `meson setup` fails with "Command `/usr/bin/cat ../VERSION` failed with status 1".
cp "${DOCA}/VERSION" ./VERSION

echo ">> [2/4] applying ${PATCH} (pure-ECN controller; touches only algo/rtt_template.c)"
patch -p1 -d app < "${PATCH}"

# Put dpacc on PATH so build_device_code.sh finds it; meson runs it (via run_command, at configure
# time) to compile the device-side DPA algo — so the patch above must come BEFORE meson setup.
export PATH="${DOCA}/tools:${PATH}"

echo ">> [3/4] meson setup (pcc only)"
meson setup app/build app -Denable_all_applications=false -Denable_pcc=true

echo ">> [4/4] ninja"
ninja -C app/build

echo
echo ">> done: $(pwd)/app/build/pcc/doca_pcc"
